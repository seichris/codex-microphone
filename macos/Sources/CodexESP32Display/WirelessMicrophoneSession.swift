import Foundation

/// Deterministic server-side lifecycle for one authenticated board connection.
/// Network callbacks call this value through a lock-owned wrapper; the state
/// itself has no timers or I/O, which keeps reconnect and stale-frame behavior
/// straightforward to test.
struct WirelessMicrophoneSession: Sendable {
    enum Phase: String, Sendable {
        case disconnected
        case authenticated
        case prepared
        case armed
        case listening
        case stopped
        case canceled

        /// Cancellation is an interruption, not a successfully stopped
        /// recording. The owner must still close its Speech receiver.
        var requiresReceiverCancellation: Bool {
            switch self {
            case .prepared, .armed, .listening, .canceled: return true
            case .disconnected, .authenticated, .stopped: return false
            }
        }
    }

    struct Snapshot: Equatable, Sendable {
        let phase: Phase
        let sessionID: UUID?
        let generation: UInt64
        let threadID: String?
        let highestSequence: UInt32?
        let acceptedFrames: UInt32
    }

    private(set) var phase: Phase = .disconnected
    private(set) var deviceID: String?
    private(set) var sessionID: UUID?
    private(set) var generation: UInt64 = 0
    private(set) var threadID: String?
    private(set) var highestSequence: UInt32?
    private(set) var acceptedFrames: UInt32 = 0

    private var expectedCredential: String
    private var expectedDeviceID: String?
    private var authenticated = false
    private var startRequestID: String?
    private var preparedResponse: WirelessMicrophoneProtocol.ControlMessage?
    private var stoppedResponse: WirelessMicrophoneProtocol.ControlMessage?

    init(expectedCredential: String, expectedDeviceID: String? = nil) {
        self.expectedCredential = expectedCredential
        self.expectedDeviceID = expectedDeviceID
    }

    var snapshot: Snapshot {
        Snapshot(phase: phase, sessionID: sessionID, generation: generation,
                 threadID: threadID, highestSequence: highestSequence,
                 acceptedFrames: acceptedFrames)
    }

    mutating func authenticate(_ message: WirelessMicrophoneProtocol.ControlMessage) throws -> WirelessMicrophoneProtocol.ControlMessage {
        guard message.type == .hello else { throw Error.notAuthenticated }
        guard constantTimeEqual(message.credential ?? "", expectedCredential),
              let messageDeviceID = message.deviceID,
              expectedDeviceID == nil || messageDeviceID == expectedDeviceID else {
            throw Error.authenticationFailed
        }
        if authenticated {
            guard message.deviceID == deviceID else { throw Error.authenticationFailed }
        } else {
            authenticated = true
            deviceID = message.deviceID
            phase = .authenticated
        }
        return WirelessMicrophoneProtocol.ControlMessage(
            .capabilities,
            format: .pcm48Mono16,
            maxFrameBytes: WirelessMicrophoneProtocol.maxAudioMessageLength,
            heartbeatSeconds: 1
        )
    }

    mutating func start(_ message: WirelessMicrophoneProtocol.ControlMessage) throws -> WirelessMicrophoneProtocol.ControlMessage {
        guard authenticated else { throw Error.notAuthenticated }
        guard message.type == .start,
              let requestID = message.requestID,
              let threadID = message.threadID else { throw Error.invalidState("start fields") }
        // A normal stop ends only the current session; the authenticated
        // connection is intentionally reusable for the next physical gesture.
        // Clear per-session responses before accepting a fresh request ID.
        guard phase != .canceled else { throw Error.invalidState("start after cancel") }
        if phase == .stopped {
            // A delayed retry is not a new physical gesture. Check before
            // dropping the completed session's idempotency key.
            guard requestID != startRequestID else { throw Error.requestIDReused }
            startRequestID = nil
            preparedResponse = nil
            stoppedResponse = nil
            sessionID = nil
            self.threadID = nil
            highestSequence = nil
            acceptedFrames = 0
            phase = .authenticated
        }
        if let existing = startRequestID {
            guard existing == requestID, self.threadID == threadID, message.transport == "wifi" else {
                throw Error.requestIDReused
            }
            if let preparedResponse { return preparedResponse }
        }
        guard phase == .authenticated else { throw Error.invalidState("start while \(phase.rawValue)") }
        startRequestID = requestID
        sessionID = UUID()
        generation &+= 1
        self.threadID = threadID
        highestSequence = nil
        acceptedFrames = 0
        phase = .prepared
        let response = WirelessMicrophoneProtocol.ControlMessage(
            .prepared,
            requestID: requestID,
            sessionID: sessionID?.uuidString,
            generation: generation,
            threadID: threadID,
            transport: "wifi",
            format: .pcm48Mono16
        )
        preparedResponse = response
        return response
    }

    mutating func commit(_ message: WirelessMicrophoneProtocol.ControlMessage) throws -> WirelessMicrophoneProtocol.ControlMessage {
        try validateSession(message)
        switch phase {
        case .prepared:
            phase = .armed
            return WirelessMicrophoneProtocol.ControlMessage(.armed, sessionID: message.sessionID, generation: generation)
        case .armed, .listening:
            return WirelessMicrophoneProtocol.ControlMessage(.armed, sessionID: message.sessionID, generation: generation)
        default:
            throw Error.invalidState("commit while \(phase.rawValue)")
        }
    }

    mutating func acceptAudio(_ frame: WirelessMicrophoneProtocol.AudioFrame) throws -> WirelessMicrophoneProtocol.ControlMessage? {
        guard phase == .armed || phase == .listening else { throw Error.invalidState("audio while \(phase.rawValue)") }
        guard frame.sessionID == sessionID else { throw Error.staleSession }
        let expected = highestSequence.map { $0 &+ 1 } ?? 0
        guard frame.sequence == expected else { throw Error.sequenceMismatch(expected: expected, received: frame.sequence) }
        guard frame.firstSample == UInt64(frame.sequence) * UInt64(WirelessMicrophoneProtocol.samplesPerFrame) else {
            throw Error.sequenceMismatch(expected: expected, received: frame.sequence)
        }
        highestSequence = frame.sequence
        acceptedFrames &+= 1
        if phase == .armed {
            phase = .listening
            return WirelessMicrophoneProtocol.ControlMessage(
                .listening,
                sessionID: sessionID?.uuidString,
                generation: generation,
                sequence: frame.sequence
            )
        }
        // Five 20 ms blocks is the initial 100 ms acknowledgement cadence.
        if frame.sequence % 5 == 4 {
            return WirelessMicrophoneProtocol.ControlMessage(
                .ack,
                sessionID: sessionID?.uuidString,
                generation: generation,
                sequence: frame.sequence
            )
        }
        return nil
    }

    mutating func stop(_ message: WirelessMicrophoneProtocol.ControlMessage) throws -> WirelessMicrophoneProtocol.ControlMessage {
        try validateSession(message)
        guard let finalSequence = message.finalSequence else { throw Error.invalidState("stop fields") }
        let expectedFinal = highestSequence.map { $0 &+ 1 } ?? 0
        guard finalSequence == expectedFinal else {
            throw Error.sequenceMismatch(expected: expectedFinal, received: finalSequence)
        }
        if let stoppedResponse { return stoppedResponse }
        guard phase == .armed || phase == .listening else { throw Error.invalidState("stop while \(phase.rawValue)") }
        phase = .stopped
        let response = WirelessMicrophoneProtocol.ControlMessage(
            .stopped,
            sessionID: sessionID?.uuidString,
            generation: generation,
            finalSequence: finalSequence
        )
        stoppedResponse = response
        return response
    }

    mutating func cancel(_ message: WirelessMicrophoneProtocol.ControlMessage) throws {
        try validateSession(message)
        guard phase != .stopped && phase != .canceled else { return }
        phase = .canceled
    }

    private func validateSession(_ message: WirelessMicrophoneProtocol.ControlMessage) throws {
        guard authenticated else { throw Error.notAuthenticated }
        guard message.sessionID == sessionID?.uuidString, message.generation == generation else {
            throw Error.staleSession
        }
    }

    enum Error: Swift.Error, Equatable, CustomStringConvertible {
        case notAuthenticated
        case authenticationFailed
        case requestIDReused
        case invalidState(String)
        case staleSession
        case sequenceMismatch(expected: UInt32, received: UInt32)

        var description: String {
            switch self {
            case .notAuthenticated: return "wireless microphone connection is not authenticated"
            case .authenticationFailed: return "wireless microphone authentication failed"
            case .requestIDReused: return "wireless microphone request ID was reused"
            case let .invalidState(state): return "wireless microphone session is in \(state)"
            case .staleSession: return "wireless microphone session is stale"
            case let .sequenceMismatch(expected, received):
                return "wireless microphone sequence mismatch (expected \(expected), received \(received))"
            }
        }
    }

    private func constantTimeEqual(_ left: String, _ right: String) -> Bool {
        let lhs = Array(left.utf8)
        let rhs = Array(right.utf8)
        var difference = lhs.count ^ rhs.count
        let count = max(lhs.count, rhs.count)
        for index in 0..<count {
            let a = index < lhs.count ? lhs[index] : 0
            let b = index < rhs.count ? rhs[index] : 0
            difference |= Int(a ^ b)
        }
        return difference == 0
    }
}
