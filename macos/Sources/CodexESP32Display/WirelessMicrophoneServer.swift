import Foundation
import Network
import Security

struct WirelessPairing: Codable, Equatable, Sendable {
    let boardID: String
    let host: String
    let port: UInt16
    let serverName: String
    let serverCertificatePEM: String
    let credential: String
}

struct WirelessPairingBundle: Codable, Equatable, Sendable {
    let boardID: String
    let host: String
    let port: UInt16
    let serverName: String
    let serverCertificatePEM: String
    let credential: String
    let serverIdentityPKCS12Base64: String?
    let serverIdentityPassphrase: String?

    init(pairing: WirelessPairing, serverIdentityPKCS12Base64: String? = nil, serverIdentityPassphrase: String? = nil) {
        self.boardID = pairing.boardID
        self.host = pairing.host
        self.port = pairing.port
        self.serverName = pairing.serverName
        self.serverCertificatePEM = pairing.serverCertificatePEM
        self.credential = pairing.credential
        self.serverIdentityPKCS12Base64 = serverIdentityPKCS12Base64
        self.serverIdentityPassphrase = serverIdentityPassphrase
    }

    var pairing: WirelessPairing {
        WirelessPairing(boardID: boardID, host: host, port: port, serverName: serverName,
                        serverCertificatePEM: serverCertificatePEM, credential: credential)
    }
}

enum WirelessMicrophoneServerError: Error, Equatable, CustomStringConvertible {
    case pairingNotConfigured
    case invalidPairing
    case listenerUnavailable(String)

    var description: String {
        switch self {
        case .pairingNotConfigured: return "Pair the board with this Mac before enabling Wi-Fi dictation."
        case .invalidPairing: return "The wireless microphone pairing bundle is invalid."
        case let .listenerUnavailable(reason): return "The wireless microphone listener is unavailable: \(reason)"
        }
    }
}

/// Stores only the non-secret pairing metadata in preferences. The credential
/// and the server identity remain in Keychain so a preferences export cannot
/// silently authorize a board connection.
final class WirelessPairingStore: @unchecked Sendable {
    static let shared = WirelessPairingStore()
    private static let service = "com.seichris.codex-esp32-display.wireless-microphone"
    private static let credentialAccount = "board-credential"
    private static let identityLabel = "Codex ESP32 Display Wireless Microphone"

    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    var pairing: WirelessPairing? {
        guard let boardID = defaults.string(forKey: "WirelessMicrophone.boardID"),
              let host = defaults.string(forKey: "WirelessMicrophone.host"),
              let serverName = defaults.string(forKey: "WirelessMicrophone.serverName"),
              let certificate = defaults.string(forKey: "WirelessMicrophone.certificatePEM"),
              let portNumber = defaults.object(forKey: "WirelessMicrophone.port") as? NSNumber,
              let credential = readCredential() else { return nil }
        let port = UInt16(clamping: portNumber.intValue)
        guard !boardID.isEmpty, !host.isEmpty, !serverName.isEmpty, !certificate.isEmpty, port != 0 else { return nil }
        return WirelessPairing(boardID: boardID, host: host, port: port, serverName: serverName,
                               serverCertificatePEM: certificate, credential: credential)
    }

    func save(_ pairing: WirelessPairing) throws {
        guard Self.valid(pairing) else { throw WirelessMicrophoneServerError.invalidPairing }
        try writeCredential(pairing.credential)
        defaults.set(pairing.boardID, forKey: "WirelessMicrophone.boardID")
        defaults.set(pairing.host, forKey: "WirelessMicrophone.host")
        defaults.set(Int(pairing.port), forKey: "WirelessMicrophone.port")
        defaults.set(pairing.serverName, forKey: "WirelessMicrophone.serverName")
        defaults.set(pairing.serverCertificatePEM, forKey: "WirelessMicrophone.certificatePEM")
    }

    func importBundle(_ data: Data) throws -> WirelessPairing {
        guard data.count <= 64 * 1024,
              let object = try? JSONSerialization.jsonObject(with: data),
              let dictionary = object as? [String: Any] else {
            throw WirelessMicrophoneServerError.invalidPairing
        }
        let allowed = Set([
            "boardID", "host", "port", "serverName", "serverCertificatePEM", "credential",
            "serverIdentityPKCS12Base64", "serverIdentityPassphrase",
        ])
        guard dictionary.keys.allSatisfy(allowed.contains) else {
            throw WirelessMicrophoneServerError.invalidPairing
        }
        let bundle: WirelessPairingBundle
        do { bundle = try JSONDecoder().decode(WirelessPairingBundle.self, from: data) }
        catch { throw WirelessMicrophoneServerError.invalidPairing }
        guard Self.valid(bundle.pairing),
              bundle.serverIdentityPKCS12Base64 != nil,
              bundle.serverIdentityPassphrase != nil else {
            throw WirelessMicrophoneServerError.invalidPairing
        }
        guard (bundle.serverIdentityPKCS12Base64 == nil) == (bundle.serverIdentityPassphrase == nil) else {
            throw WirelessMicrophoneServerError.invalidPairing
        }
        if let encodedIdentity = bundle.serverIdentityPKCS12Base64,
           let passphrase = bundle.serverIdentityPassphrase,
           let identity = Data(base64Encoded: encodedIdentity) {
            try importServerIdentity(identity, password: passphrase)
        } else if bundle.serverIdentityPKCS12Base64 != nil {
            throw WirelessMicrophoneServerError.invalidPairing
        }
        try save(bundle.pairing)
        return bundle.pairing
    }

    func remove() {
        let query: [CFString: Any] = [
            kSecClass: kSecClassGenericPassword,
            kSecAttrService: Self.service,
            kSecAttrAccount: Self.credentialAccount,
        ]
        SecItemDelete(query as CFDictionary)
        SecItemDelete([
            kSecClass: kSecClassIdentity,
            kSecAttrLabel: Self.identityLabel,
        ] as CFDictionary)
        for key in ["WirelessMicrophone.boardID", "WirelessMicrophone.host", "WirelessMicrophone.port",
                    "WirelessMicrophone.serverName", "WirelessMicrophone.certificatePEM"] {
            defaults.removeObject(forKey: key)
        }
    }

    func makeCredential() throws -> String {
        var bytes = [UInt8](repeating: 0, count: 32)
        guard SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes) == errSecSuccess else {
            throw WirelessMicrophoneServerError.invalidPairing
        }
        return bytes.map { String(format: "%02x", $0) }.joined()
    }

    /// Imports a PKCS#12 identity generated by the local pairing workflow.
    /// The password is never logged or persisted by this class.
    func importServerIdentity(_ data: Data, password: String) throws {
        // SecPKCS12Import imports the identity into the user's default
        // keychain and returns a reference to that item.  Calling SecItemAdd
        // with kSecClassIdentity and kSecValueRef is not a valid way to copy
        // that reference (it returns errSecParam), which used to make every
        // otherwise-valid pairing bundle fail to import.  Remove the previous
        // generated identity first so a replacement cannot leave an older
        // certificate selected by the label lookup below.
        SecItemDelete([
            kSecClass: kSecClassIdentity,
            kSecAttrLabel: Self.identityLabel,
        ] as CFDictionary)

        var items: CFArray?
        let options = [kSecImportExportPassphrase as String: password]
        let status = SecPKCS12Import(data as CFData, options as CFDictionary, &items)
        guard status == errSecSuccess,
              let array = items as? [[String: Any]],
              array.first?[kSecImportItemIdentity as String] != nil else {
            throw WirelessMicrophoneServerError.invalidPairing
        }

        // Confirm that the imported item is discoverable through the same
        // stable label used by the listener after a relaunch.
        var imported: CFTypeRef?
        let query: [CFString: Any] = [
            kSecClass: kSecClassIdentity,
            kSecAttrLabel: Self.identityLabel,
            kSecReturnRef: true,
            kSecMatchLimit: kSecMatchLimitOne,
        ]
        guard SecItemCopyMatching(query as CFDictionary, &imported) == errSecSuccess,
              let imported,
              CFGetTypeID(imported) == SecIdentityGetTypeID() else {
            throw WirelessMicrophoneServerError.invalidPairing
        }
    }

    func serverIdentity() -> SecIdentity? {
        var result: CFTypeRef?
        let query: [CFString: Any] = [
            kSecClass: kSecClassIdentity,
            kSecAttrLabel: Self.identityLabel,
            kSecReturnRef: true,
            kSecMatchLimit: kSecMatchLimitOne,
        ]
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess else { return nil }
        guard let result, CFGetTypeID(result) == SecIdentityGetTypeID() else { return nil }
        return (result as! SecIdentity)
    }

    private func readCredential() -> String? {
        var result: CFTypeRef?
        let query: [CFString: Any] = [
            kSecClass: kSecClassGenericPassword,
            kSecAttrService: Self.service,
            kSecAttrAccount: Self.credentialAccount,
            kSecReturnData: true,
            kSecMatchLimit: kSecMatchLimitOne,
        ]
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess,
              let data = result as? Data,
              let value = String(data: data, encoding: .utf8),
              value.count == 64 else { return nil }
        return value
    }

    private func writeCredential(_ value: String) throws {
        let query: [CFString: Any] = [
            kSecClass: kSecClassGenericPassword,
            kSecAttrService: Self.service,
            kSecAttrAccount: Self.credentialAccount,
        ]
        SecItemDelete(query as CFDictionary)
        var item = query
        item[kSecValueData] = Data(value.utf8)
        item[kSecAttrAccessible] = kSecAttrAccessibleAfterFirstUnlock
        guard SecItemAdd(item as CFDictionary, nil) == errSecSuccess else {
            throw WirelessMicrophoneServerError.invalidPairing
        }
    }

    private static func valid(_ pairing: WirelessPairing) -> Bool {
        pairing.boardID.count >= 1 && pairing.boardID.count <= 96
            && pairing.host.count >= 1 && pairing.host.count <= 255
            && pairing.serverName.count >= 1 && pairing.serverName.count <= 255
            && pairing.serverCertificatePEM.count >= 32 && pairing.serverCertificatePEM.count <= 16_384
            && pairing.serverCertificatePEM.contains("-----BEGIN CERTIFICATE-----")
            && pairing.serverCertificatePEM.contains("-----END CERTIFICATE-----")
            && !pairing.serverCertificatePEM.contains("\0")
            && pairing.credential.count == 64
            && pairing.credential.allSatisfy { $0.isHexDigit }
            && !pairing.boardID.contains("\0") && !pairing.host.contains("\0")
            && !pairing.serverName.contains("\0")
            && pairing.port != 0
    }
}

final class WirelessMicrophoneServer: @unchecked Sendable {
    enum State: Equatable, Sendable {
        case stopped
        case starting
        case ready(port: UInt16)
        case failed(String)
    }

    struct Configuration: Sendable {
        let port: UInt16
        let pairingStore: WirelessPairingStore

        init(port: UInt16 = 5_181, pairingStore: WirelessPairingStore = .shared) {
            self.port = port
            self.pairingStore = pairingStore
        }
    }

    private final class ConnectionContext: @unchecked Sendable {
        enum StartDecision: Sendable {
            case pending
            case accepted
        }

        let connection: NWConnection
        var session: WirelessMicrophoneSession
        var authenticated = false
        var fragmentedMessage = Data()
        var fragmentedOpcode: NWProtocolWebSocket.Opcode?
        var authDeadline: DispatchWorkItem?
        var startRequestID: String?
        var startDecision: StartDecision?
        var suppressFailureCallback = false
        var failureCallbackReported = false

        init(connection: NWConnection, credential: String, deviceID: String) {
            self.connection = connection
            self.session = WirelessMicrophoneSession(
                expectedCredential: credential,
                expectedDeviceID: deviceID
            )
        }
    }

    private let configuration: Configuration
    private let queue = DispatchQueue(label: "codex.wireless-microphone.server")
    private let queueKey = DispatchSpecificKey<UInt8>()
    private var listener: NWListener?
    private var connection: ConnectionContext?
    private var storedState: State = .stopped

    var state: State { onQueue { storedState } }

    // Listener, connection, readiness, and pairing replacement share one
    // executor. Main-actor callers never race Network.framework callbacks.
    private func onQueue<T>(_ body: () throws -> T) rethrows -> T {
        if DispatchQueue.getSpecific(key: queueKey) != nil { return try body() }
        return try queue.sync(execute: body)
    }

    /// Called after the board's start is accepted and before `prepared` is sent.
    var onStart: (@Sendable (String, UUID) async -> Bool)?
    /// Called after a valid stop, before `stopped` is sent to the board.
    var onStop: (@Sendable (String, UUID, UInt32) async -> Void)?
    /// Called only after the session is armed and a frame has passed validation.
    var onAudioFrame: (@Sendable (WirelessMicrophoneProtocol.AudioFrame) -> Void)?
    /// Called when an active session ends without a valid board stop. The
    /// receiver must cancel local Speech capture and keep any partial text for
    /// review instead of treating the interruption as a successful draft.
    var onSessionFailure: (@Sendable (String, UUID, String) async -> Void)?
    var onStateChange: (@Sendable (State) -> Void)?

    init(configuration: Configuration = Configuration()) {
        self.configuration = configuration
        queue.setSpecific(key: queueKey, value: 1)
    }

    var isReady: Bool {
        if case .ready = state { return true }
        return false
    }

    var configurationPairing: WirelessPairing? { onQueue { configuration.pairingStore.pairing } }

    func importPairingBundle(_ data: Data) throws -> WirelessPairing {
        try onQueue {
            // Fail closed before touching identity/credential storage. A
            // partially failed import must not leave the old listener alive.
            stopOnQueue()
            return try configuration.pairingStore.importBundle(data)
        }
    }

    func removePairing() {
        onQueue {
            stopOnQueue()
            configuration.pairingStore.remove()
        }
    }

    /// Cancels an in-flight board session when a local UI/legacy command ends
    /// recognition. A normal board-originated stop has already moved the
    /// session to `.stopped`, so this is a no-op in that case.
    func cancelActiveSession() {
        queue.async {
            guard let context = self.connection else { return }
            switch context.session.phase {
            case .prepared, .armed, .listening:
                context.suppressFailureCallback = true
                context.connection.cancel()
                if self.connection === context { self.connection = nil }
            default:
                break
            }
        }
    }

    func start() throws { try onQueue { try startOnQueue() } }

    private func startOnQueue() throws {
        guard listener == nil else { return }
        guard let identity = configuration.pairingStore.serverIdentity(),
              let pairing = configuration.pairingStore.pairing else {
            storedState = .failed(WirelessMicrophoneServerError.pairingNotConfigured.description)
            onStateChange?(state)
            throw WirelessMicrophoneServerError.pairingNotConfigured
        }
        guard let port = NWEndpoint.Port(rawValue: pairing.port) else {
            storedState = .failed("invalid listener port")
            onStateChange?(state)
            throw WirelessMicrophoneServerError.listenerUnavailable("invalid port")
        }

        let tlsOptions = NWProtocolTLS.Options()
        guard let localIdentity = sec_identity_create(identity) else {
            storedState = .failed("could not load the paired TLS identity")
            onStateChange?(state)
            throw WirelessMicrophoneServerError.listenerUnavailable("invalid TLS identity")
        }
        sec_protocol_options_set_local_identity(tlsOptions.securityProtocolOptions, localIdentity)
        let parameters = NWParameters(tls: tlsOptions)
        parameters.serviceClass = .interactiveVoice
        parameters.allowLocalEndpointReuse = true
        let webSocket = NWProtocolWebSocket.Options()
        webSocket.autoReplyPing = true
        webSocket.maximumMessageSize = WirelessMicrophoneProtocol.maxControlMessageLength
        webSocket.setSubprotocols([WirelessMicrophoneProtocol.subprotocol])
        webSocket.setClientRequestHandler(queue) { subprotocols, _ in
            guard subprotocols.contains(WirelessMicrophoneProtocol.subprotocol) else {
                return NWProtocolWebSocket.Response(status: .reject, subprotocol: nil)
            }
            return NWProtocolWebSocket.Response(status: .accept, subprotocol: WirelessMicrophoneProtocol.subprotocol)
        }
        parameters.defaultProtocolStack.applicationProtocols.insert(webSocket, at: 0)

        let listener: NWListener
        do { listener = try NWListener(using: parameters, on: port) }
        catch {
            storedState = .failed(error.localizedDescription)
            onStateChange?(state)
            throw WirelessMicrophoneServerError.listenerUnavailable(error.localizedDescription)
        }
        self.listener = listener
        storedState = .starting
        onStateChange?(state)
        listener.stateUpdateHandler = { [weak self, weak listener] newState in
            guard let self, let listener else { return }
            self.queue.async {
                // A canceled previous listener must not reset a replacement's
                // readiness or discard its reference after a pairing import.
                guard self.listener === listener else { return }
                switch newState {
                case .ready:
                    self.storedState = .ready(port: pairing.port)
                case let .failed(error):
                    self.storedState = .failed(error.localizedDescription)
                    self.listener = nil
                case .cancelled:
                    self.storedState = .stopped
                default: break
                }
                self.onStateChange?(self.state)
            }
        }
        listener.newConnectionHandler = { [weak self, weak listener] connection in
            guard let self else { connection.cancel(); return }
            self.queue.async {
                guard let listener, self.listener === listener else { connection.cancel(); return }
                self.accept(connection)
            }
        }
        listener.start(queue: queue)
    }

    func stop() {
        queue.async { self.stopOnQueue() }
    }

    private func stopOnQueue() {
        // Cancellation callbacks hold context weakly. Notify the receiver
        // before revocation drops the last strong reference to its session.
        if let context = connection {
            reportFailureIfNeeded(context, reason: "Wireless microphone listener stopped or pairing changed.")
        }
        connection?.authDeadline?.cancel()
        connection?.connection.cancel()
        connection = nil
        listener?.cancel()
        listener = nil
        storedState = .stopped
        onStateChange?(state)
    }

    private func accept(_ connection: NWConnection) {
        // One active board is deliberate in v1. Do not let an untrusted second
        // connection interrupt an acknowledged recording.
        guard self.connection == nil else { connection.cancel(); return }
        guard let pairing = configuration.pairingStore.pairing else { connection.cancel(); return }
        let context = ConnectionContext(
            connection: connection,
            credential: pairing.credential,
            deviceID: pairing.boardID
        )
        self.connection = context
        // Reserve the single-board slot only for a bounded handshake. Waiting
        // until .ready to start a timer lets a silent TLS peer occupy it forever.
        scheduleAuthenticationDeadline(context, after: 10)
        connection.stateUpdateHandler = { [weak self, weak context] newState in
            guard let self, let context else { return }
            self.queue.async {
                guard self.connection === context else { return }
                if case .ready = newState {
                    self.startReceiving(context)
                    self.scheduleAuthenticationDeadline(context, after: 3)
                } else if case .failed = newState {
                    self.reportFailureIfNeeded(context, reason: "Wireless microphone connection failed.")
                    if self.connection === context { self.connection = nil }
                } else if case .cancelled = newState {
                    self.reportFailureIfNeeded(context, reason: "Wireless microphone connection closed.")
                    if self.connection === context { self.connection = nil }
                }
            }
        }
        connection.start(queue: queue)
    }

    private func scheduleAuthenticationDeadline(_ context: ConnectionContext, after seconds: Double) {
        context.authDeadline?.cancel()
        let deadline = DispatchWorkItem { [weak self, weak context] in
            guard let self, let context, self.connection === context, !context.authenticated else { return }
            self.fail(context, "Wireless microphone handshake or authentication timed out.")
        }
        context.authDeadline = deadline
        queue.asyncAfter(deadline: .now() + seconds, execute: deadline)
    }

    private func startReceiving(_ context: ConnectionContext) {
        context.connection.receiveMessage { [weak self, weak context] data, contentContext, isComplete, error in
            guard let self, let context else { return }
            self.queue.async {
                guard self.connection === context else { return }
                if let error { self.fail(context, error.localizedDescription); return }
                guard let metadata = contentContext?.protocolMetadata(definition: NWProtocolWebSocket.definition)
                    as? NWProtocolWebSocket.Metadata else {
                    self.fail(context, "missing WebSocket message metadata"); return
                }
                if let data { self.handle(context, data: data, opcode: metadata.opcode, isComplete: isComplete) }
                else if isComplete {
                    if metadata.opcode == .close {
                        self.fail(context, "Wireless microphone connection closed.")
                    } else if metadata.opcode == .text || metadata.opcode == .binary {
                        self.fail(context, "empty WebSocket message")
                    }
                }
                if self.connection === context { self.startReceiving(context) }
            }
        }
    }

    private func handle(_ context: ConnectionContext, data: Data, opcode: NWProtocolWebSocket.Opcode, isComplete: Bool) {
        if !isComplete {
            let limit = (opcode == .binary || context.fragmentedOpcode == .binary)
                ? WirelessMicrophoneProtocol.maxAudioMessageLength
                : WirelessMicrophoneProtocol.maxControlMessageLength
            guard context.fragmentedMessage.count + data.count <= limit else {
                fail(context, "fragmented message too large"); return
            }
            if context.fragmentedMessage.isEmpty { context.fragmentedOpcode = opcode }
            context.fragmentedMessage.append(data)
            return
        }
        let payload: Data
        let payloadOpcode: NWProtocolWebSocket.Opcode
        if context.fragmentedMessage.isEmpty {
            payload = data
            payloadOpcode = opcode
        } else {
            let limit = context.fragmentedOpcode == .binary
                ? WirelessMicrophoneProtocol.maxAudioMessageLength
                : WirelessMicrophoneProtocol.maxControlMessageLength
            guard data.count <= limit - context.fragmentedMessage.count else {
                fail(context, "fragmented message too large"); return
            }
            context.fragmentedMessage.append(data)
            payload = context.fragmentedMessage
            payloadOpcode = context.fragmentedOpcode ?? opcode
            context.fragmentedMessage.removeAll(keepingCapacity: false)
            context.fragmentedOpcode = nil
        }
        switch payloadOpcode {
        case .text: handleControl(context, data: payload)
        case .binary: handleAudio(context, data: payload)
        case .close: context.connection.cancel()
        default: break // pings/pongs are handled by Network.framework
        }
    }

    private func handleControl(_ context: ConnectionContext, data: Data) {
        do {
            let message = try WirelessMicrophoneProtocol.decodeControl(data)
            switch message.type {
            case .hello:
                let capabilities = try context.session.authenticate(message)
                context.authenticated = true
                context.authDeadline?.cancel()
                send(context, capabilities)
            case .start:
                let before = context.session.snapshot
                let existingRequest = context.startRequestID
                let prepared = try context.session.start(message)
                guard let sessionID = prepared.sessionID.flatMap(UUID.init(uuidString:)),
                      let threadID = prepared.threadID else { throw WirelessMicrophoneSession.Error.invalidState("prepared fields") }
                let isNewSession = before.phase == .stopped || existingRequest != message.requestID
                if !isNewSession {
                    // A duplicate start is answered only after the original
                    // receiver preparation completes. Never run the Speech
                    // setup twice or arm the board before its first call is
                    // ready.
                    if context.startDecision == .accepted { self.send(context, prepared) }
                    return
                }
                context.startRequestID = message.requestID
                context.startDecision = .pending
                Task {
                    let accepted = await self.onStart?(threadID, sessionID) ?? false
                    self.queue.async {
                        guard self.connection === context,
                              context.session.phase == .prepared,
                              context.startDecision == .pending else {
                            // The disconnect callback may have run before
                            // onStart finished preparing Speech. Clean up
                            // again now, scoped to this obsolete session UUID.
                            if accepted {
                                let callback = self.onSessionFailure
                                Task { await callback?(threadID, sessionID, "Wireless microphone disconnected during preparation.") }
                            }
                            return
                        }
                        if accepted {
                            context.startDecision = .accepted
                            self.send(context, prepared)
                        } else {
                            context.startDecision = nil
                            self.sendError(context, code: "receiver_unavailable", message: "Speech receiver is not ready."); context.connection.cancel()
                        }
                    }
                }
            case .commit:
                send(context, try context.session.commit(message))
            case .stop:
                guard let sessionID = message.sessionID.flatMap(UUID.init(uuidString:)),
                      let threadID = context.session.threadID,
                      let finalSequence = message.finalSequence else { throw WirelessMicrophoneSession.Error.invalidState("stop fields") }
                let before = context.session.snapshot
                let stopped = try context.session.stop(message)
                if before.phase == .stopped {
                    // The state machine makes stop idempotent. Do not invoke
                    // the receiver callback twice if a retransmitted stop
                    // arrives while the first acknowledgement is draining.
                    send(context, stopped)
                    return
                }
                Task {
                    await self.onStop?(threadID, sessionID, finalSequence)
                    self.queue.async { if self.connection === context { self.send(context, stopped) } }
                }
            case .cancel:
                try context.session.cancel(message)
                reportFailureIfNeeded(context, reason: "Wireless microphone session canceled by the board.")
                context.connection.cancel()
            default:
                throw WirelessMicrophoneSession.Error.invalidState("unexpected \(message.type.rawValue)")
            }
        } catch {
            sendError(context, code: "protocol_error", message: error.localizedDescription)
            context.connection.cancel()
        }
    }

    private func handleAudio(_ context: ConnectionContext, data: Data) {
        do {
            let frame = try WirelessMicrophoneProtocol.decodeAudioFrame(data)
            let response = try context.session.acceptAudio(frame)
            onAudioFrame?(frame)
            if let response { send(context, response) }
        } catch {
            sendError(context, code: "audio_rejected", message: error.localizedDescription)
            context.connection.cancel()
        }
    }

    private func send(_ context: ConnectionContext, _ message: WirelessMicrophoneProtocol.ControlMessage) {
        guard let data = try? WirelessMicrophoneProtocol.encodeControl(message) else { fail(context, "could not encode control message"); return }
        let metadata = NWProtocolWebSocket.Metadata(opcode: .text)
        let content = NWConnection.ContentContext(identifier: "codex-microphone-control", metadata: [metadata])
        context.connection.send(content: data, contentContext: content, isComplete: true, completion: .contentProcessed { [weak self, weak context] error in
            guard let self, let context, let error else { return }
            self.queue.async { self.fail(context, error.localizedDescription) }
        })
    }

    private func sendError(_ context: ConnectionContext, code: String, message: String) {
        send(context, .init(.error, errorCode: code, message: String(message.prefix(512))))
    }

    private func fail(_ context: ConnectionContext, _ reason: String) {
        reportFailureIfNeeded(context, reason: reason)
        context.authDeadline?.cancel()
        context.connection.cancel()
        if connection === context { connection = nil }
        onStateChange?(.failed(reason))
    }

    private func reportFailureIfNeeded(_ context: ConnectionContext, reason: String) {
        guard !context.suppressFailureCallback, !context.failureCallbackReported else { return }
        let snapshot = context.session.snapshot
        guard snapshot.phase.requiresReceiverCancellation,
              let threadID = snapshot.threadID,
              let sessionID = snapshot.sessionID else { return }
        context.failureCallbackReported = true
        let callback = onSessionFailure
        Task { await callback?(threadID, sessionID, reason) }
    }
}
