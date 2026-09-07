import Foundation

/// The wire contract shared by the ESP32 client and the native macOS receiver.
///
/// Control messages are UTF-8 JSON WebSocket messages. Audio is one complete
/// PCM block per binary WebSocket message. Keeping the framing independent of
/// either endpoint makes it possible to test the safety boundaries without
/// requiring a microphone, a network, or Speech permission.
enum WirelessMicrophoneProtocol {
    static let version = 1
    static let subprotocol = "codex-microphone.v1"
    static let sampleRate = 48_000
    static let channels = 1
    static let bitsPerSample = 16
    static let samplesPerFrame = 960 // 20 ms at 48 kHz
    static let pcmBytesPerFrame = samplesPerFrame * 2
    static let audioHeaderLength = 36
    static let maxAudioMessageLength = audioHeaderLength + pcmBytesPerFrame
    static let maxControlMessageLength = 4 * 1024
    static let audioMagic = Data([0x43, 0x4D, 0x49, 0x43]) // "CMIC"

    enum ProtocolError: Error, Equatable, CustomStringConvertible {
        case messageTooLarge
        case malformedJSON
        case unknownControlField(String)
        case invalidControl(String)
        case invalidAudio(String)

        var description: String {
            switch self {
            case .messageTooLarge: return "message exceeds the wireless microphone limit"
            case .malformedJSON: return "control message is not valid JSON"
            case let .unknownControlField(field): return "unknown control field: \(field)"
            case let .invalidControl(reason): return "invalid control message: \(reason)"
            case let .invalidAudio(reason): return "invalid audio frame: \(reason)"
            }
        }
    }

    enum ControlType: String, Codable, Equatable, Sendable {
        case hello
        case capabilities
        case start
        case prepared
        case commit
        case armed
        case listening
        case ack
        case stop
        case stopped
        case cancel
        case error
    }

    struct AudioFormat: Codable, Equatable, Sendable {
        let sampleRate: Int
        let channels: Int
        let bitsPerSample: Int
        let samplesPerFrame: Int

        static let pcm48Mono16 = AudioFormat(
            sampleRate: WirelessMicrophoneProtocol.sampleRate,
            channels: WirelessMicrophoneProtocol.channels,
            bitsPerSample: WirelessMicrophoneProtocol.bitsPerSample,
            samplesPerFrame: WirelessMicrophoneProtocol.samplesPerFrame
        )
    }

    /// Optional fields are encoded only when they apply to the message type.
    /// This avoids accepting a valid command with unrelated, stale fields.
    struct ControlMessage: Codable, Equatable, Sendable {
        let type: ControlType
        let requestID: String?
        let sessionID: String?
        let generation: UInt64?
        let threadID: String?
        let transport: String?
        let deviceID: String?
        let credential: String?
        let sequence: UInt32?
        let finalSequence: UInt32?
        let errorCode: String?
        let message: String?
        let format: AudioFormat?
        let maxFrameBytes: Int?
        let heartbeatSeconds: Int?

        init(
            _ type: ControlType,
            requestID: String? = nil,
            sessionID: String? = nil,
            generation: UInt64? = nil,
            threadID: String? = nil,
            transport: String? = nil,
            deviceID: String? = nil,
            credential: String? = nil,
            sequence: UInt32? = nil,
            finalSequence: UInt32? = nil,
            errorCode: String? = nil,
            message: String? = nil,
            format: AudioFormat? = nil,
            maxFrameBytes: Int? = nil,
            heartbeatSeconds: Int? = nil
        ) {
            self.type = type
            self.requestID = requestID
            self.sessionID = sessionID
            self.generation = generation
            self.threadID = threadID
            self.transport = transport
            self.deviceID = deviceID
            self.credential = credential
            self.sequence = sequence
            self.finalSequence = finalSequence
            self.errorCode = errorCode
            self.message = message
            self.format = format
            self.maxFrameBytes = maxFrameBytes
            self.heartbeatSeconds = heartbeatSeconds
        }

        enum CodingKeys: String, CodingKey, CaseIterable {
            case type, version, requestID, sessionID, generation, threadID, transport
            case deviceID, credential, sequence, finalSequence, errorCode, message
            case format, maxFrameBytes, heartbeatSeconds
        }

        init(from decoder: Decoder) throws {
            let container = try decoder.container(keyedBy: CodingKeys.self)
            guard let version = try container.decodeIfPresent(Int.self, forKey: .version) else {
                throw ProtocolError.invalidControl("version is required")
            }
            self.init(
                try container.decode(ControlType.self, forKey: .type),
                requestID: try container.decodeIfPresent(String.self, forKey: .requestID),
                sessionID: try container.decodeIfPresent(String.self, forKey: .sessionID),
                generation: try container.decodeIfPresent(UInt64.self, forKey: .generation),
                threadID: try container.decodeIfPresent(String.self, forKey: .threadID),
                transport: try container.decodeIfPresent(String.self, forKey: .transport),
                deviceID: try container.decodeIfPresent(String.self, forKey: .deviceID),
                credential: try container.decodeIfPresent(String.self, forKey: .credential),
                sequence: try container.decodeIfPresent(UInt32.self, forKey: .sequence),
                finalSequence: try container.decodeIfPresent(UInt32.self, forKey: .finalSequence),
                errorCode: try container.decodeIfPresent(String.self, forKey: .errorCode),
                message: try container.decodeIfPresent(String.self, forKey: .message),
                format: try container.decodeIfPresent(AudioFormat.self, forKey: .format),
                maxFrameBytes: try container.decodeIfPresent(Int.self, forKey: .maxFrameBytes),
                heartbeatSeconds: try container.decodeIfPresent(Int.self, forKey: .heartbeatSeconds)
            )
            guard version == WirelessMicrophoneProtocol.version else {
                throw ProtocolError.invalidControl("unsupported version")
            }
            try validate()
        }

        func encode(to encoder: Encoder) throws {
            try validate()
            var container = encoder.container(keyedBy: CodingKeys.self)
            try container.encode(type, forKey: .type)
            try container.encode(WirelessMicrophoneProtocol.version, forKey: .version)
            try container.encodeIfPresent(requestID, forKey: .requestID)
            try container.encodeIfPresent(sessionID, forKey: .sessionID)
            try container.encodeIfPresent(generation, forKey: .generation)
            try container.encodeIfPresent(threadID, forKey: .threadID)
            try container.encodeIfPresent(transport, forKey: .transport)
            try container.encodeIfPresent(deviceID, forKey: .deviceID)
            try container.encodeIfPresent(credential, forKey: .credential)
            try container.encodeIfPresent(sequence, forKey: .sequence)
            try container.encodeIfPresent(finalSequence, forKey: .finalSequence)
            try container.encodeIfPresent(errorCode, forKey: .errorCode)
            try container.encodeIfPresent(message, forKey: .message)
            try container.encodeIfPresent(format, forKey: .format)
            try container.encodeIfPresent(maxFrameBytes, forKey: .maxFrameBytes)
            try container.encodeIfPresent(heartbeatSeconds, forKey: .heartbeatSeconds)
        }

        func validate() throws {
            let requiredSession = [ControlType.prepared, .commit, .armed, .listening, .ack, .stop, .stopped, .cancel]
            if requiredSession.contains(type) {
                guard let sessionID, UUID(uuidString: sessionID) != nil else {
                    throw ProtocolError.invalidControl("sessionID is required")
                }
                guard let generation, generation > 0 else {
                    throw ProtocolError.invalidControl("generation is required")
                }
            }
            switch type {
            case .hello:
                guard validText(deviceID, maximum: 96), validText(credential, maximum: 256) else {
                    throw ProtocolError.invalidControl("hello requires deviceID and credential")
                }
            case .capabilities:
                guard format == .pcm48Mono16, maxFrameBytes == maxAudioMessageLength else {
                    throw ProtocolError.invalidControl("unsupported capabilities")
                }
            case .start:
                guard validText(requestID, maximum: 96), validText(threadID, maximum: 160), transport == "wifi" else {
                    throw ProtocolError.invalidControl("start requires requestID, threadID, and wifi transport")
                }
            case .prepared:
                guard validText(requestID, maximum: 96), format == .pcm48Mono16 else {
                    throw ProtocolError.invalidControl("prepared requires requestID and PCM format")
                }
            case .commit, .armed:
                break
            case .listening:
                guard sequence != nil else { throw ProtocolError.invalidControl("listening requires sequence") }
            case .ack:
                guard sequence != nil else { throw ProtocolError.invalidControl("ack requires sequence") }
            case .stop:
                guard finalSequence != nil else { throw ProtocolError.invalidControl("stop requires finalSequence") }
            case .stopped:
                guard finalSequence != nil else { throw ProtocolError.invalidControl("stopped requires finalSequence") }
            case .cancel:
                guard validText(errorCode, maximum: 64) else { throw ProtocolError.invalidControl("cancel requires errorCode") }
            case .error:
                guard validText(errorCode, maximum: 64), validText(message, maximum: 512) else {
                    throw ProtocolError.invalidControl("error requires bounded errorCode and message")
                }
            }
        }

        private func validText(_ value: String?, maximum: Int) -> Bool {
            guard let value, !value.isEmpty, value.count <= maximum else { return false }
            return !value.unicodeScalars.contains(where: { $0.value == 0 })
        }
    }

    struct AudioFrame: Equatable, Sendable {
        let sessionID: UUID
        let sequence: UInt32
        let firstSample: UInt64
        let pcm: Data
    }

    static func encodeControl(_ message: ControlMessage) throws -> Data {
        let data = try JSONEncoder().encode(message)
        guard data.count <= maxControlMessageLength else { throw ProtocolError.messageTooLarge }
        return data
    }

    static func decodeControl(_ data: Data) throws -> ControlMessage {
        guard data.count <= maxControlMessageLength else { throw ProtocolError.messageTooLarge }
        guard let object = try? JSONSerialization.jsonObject(with: data),
              let dictionary = object as? [String: Any] else {
            throw ProtocolError.malformedJSON
        }
        let allowed = Set(ControlMessage.CodingKeys.allCases.map(\.stringValue))
        for key in dictionary.keys where !allowed.contains(key) {
            throw ProtocolError.unknownControlField(key)
        }
        do { return try JSONDecoder().decode(ControlMessage.self, from: data) }
        catch let error as ProtocolError { throw error }
        catch { throw ProtocolError.malformedJSON }
    }

    static func encodeAudioFrame(sessionID: UUID, sequence: UInt32, firstSample: UInt64, pcm: Data) throws -> Data {
        guard pcm.count == pcmBytesPerFrame else { throw ProtocolError.invalidAudio("expected one 20 ms PCM block") }
        var output = Data(capacity: maxAudioMessageLength)
        output.append(audioMagic)
        output.append(1) // framing version, independent of JSON version
        output.append(0) // flags; v1 has no optional flags
        appendUInt16BE(UInt16(audioHeaderLength), to: &output)
        var uuid = sessionID.uuid
        withUnsafeBytes(of: &uuid) { output.append(contentsOf: $0) }
        appendUInt32BE(sequence, to: &output)
        appendUInt64BE(firstSample, to: &output)
        output.append(pcm)
        return output
    }

    static func decodeAudioFrame(_ data: Data) throws -> AudioFrame {
        guard data.count == maxAudioMessageLength else {
            throw ProtocolError.invalidAudio("unexpected message length")
        }
        guard data.prefix(audioMagic.count) == audioMagic else {
            throw ProtocolError.invalidAudio("bad magic")
        }
        guard data[4] == 1, data[5] == 0 else { throw ProtocolError.invalidAudio("unsupported version or flags") }
        guard readUInt16BE(data, at: 6) == audioHeaderLength else {
            throw ProtocolError.invalidAudio("bad header length")
        }
        let uuidData = data.subdata(in: 8..<24)
        let sessionID = uuidData.withUnsafeBytes { UUID(uuid: $0.loadUnaligned(as: uuid_t.self)) }
        let sequence = readUInt32BE(data, at: 24)
        let firstSample = readUInt64BE(data, at: 28)
        return AudioFrame(sessionID: sessionID, sequence: sequence, firstSample: firstSample,
                          pcm: data.subdata(in: audioHeaderLength..<data.count))
    }

    private static func appendUInt16BE(_ value: UInt16, to data: inout Data) {
        data.append(UInt8(value >> 8)); data.append(UInt8(value & 0xFF))
    }

    private static func appendUInt32BE(_ value: UInt32, to data: inout Data) {
        data.append(UInt8((value >> 24) & 0xFF)); data.append(UInt8((value >> 16) & 0xFF))
        data.append(UInt8((value >> 8) & 0xFF)); data.append(UInt8(value & 0xFF))
    }

    private static func appendUInt64BE(_ value: UInt64, to data: inout Data) {
        for shift in stride(from: 56, through: 0, by: -8) { data.append(UInt8((value >> UInt64(shift)) & 0xFF)) }
    }

    private static func readUInt16BE(_ data: Data, at offset: Int) -> Int {
        Int(data[offset]) << 8 | Int(data[offset + 1])
    }

    private static func readUInt32BE(_ data: Data, at offset: Int) -> UInt32 {
        UInt32(data[offset]) << 24 | UInt32(data[offset + 1]) << 16
            | UInt32(data[offset + 2]) << 8 | UInt32(data[offset + 3])
    }

    private static func readUInt64BE(_ data: Data, at offset: Int) -> UInt64 {
        var result: UInt64 = 0
        for index in 0..<8 { result = (result << 8) | UInt64(data[offset + index]) }
        return result
    }
}
