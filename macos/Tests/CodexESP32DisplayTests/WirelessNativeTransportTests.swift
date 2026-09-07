import Foundation
import Network
import Security
import XCTest
@testable import CodexESP32Display

/// Real Network.framework TLS + WebSocket connections, with production framing
/// and server callbacks. Synthetic PCM only; these are not ESP32/radio/Speech tests.
final class WirelessNativeTransportTests: XCTestCase {
    private typealias Wire = WirelessMicrophoneProtocol
    private let threadID = "01a06f9d-249c-7f43-b019-95324c366b8c"

    private enum Failure: Error { case setup(String), timeout, closed }

    private final class IdentityFixture: @unchecked Sendable {
        let directory: URL
        let keychain: SecKeychain
        let identity: SecIdentity
        let certificate: SecCertificate
        let pairing: WirelessPairing

        init() throws {
            let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700])
            let password = UUID().uuidString
            var createdKeychain: SecKeychain?
            let status = directory.appendingPathComponent("fixture.keychain").path.withCString { path in
                password.withCString { bytes in
                    SecKeychainCreate(path, UInt32(password.utf8.count), bytes, false, nil, &createdKeychain)
                }
            }
            guard status == errSecSuccess, let keychain = createdKeychain else {
                try? FileManager.default.removeItem(at: directory)
                throw Failure.setup("temporary keychain: \(status)")
            }
            do {
                let configuration = directory.appendingPathComponent("openssl.cnf")
                try """
                [req]
                distinguished_name=dn
                x509_extensions=extensions
                prompt=no
                [dn]
                CN=localhost
                [extensions]
                subjectAltName=DNS:localhost
                basicConstraints=critical,CA:TRUE
                keyUsage=critical,digitalSignature,keyEncipherment,keyCertSign
                extendedKeyUsage=serverAuth
                """.write(to: configuration, atomically: true, encoding: .utf8)
                let pem = directory.appendingPathComponent("certificate.pem")
                let key = directory.appendingPathComponent("key.pem")
                let p12 = directory.appendingPathComponent("identity.p12")
                try Self.openssl(["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "1",
                    "-config", configuration.path, "-keyout", key.path, "-out", pem.path])
                // Synthetic temporary import fixture, not a change to production
                // pairing or TLS ciphers. Compatible with macOS PKCS#12 import.
                try Self.openssl(["pkcs12", "-export", "-inkey", key.path, "-in", pem.path,
                    "-out", p12.path, "-passout", "pass:\(password)",
                    "-keypbe", "PBE-SHA1-3DES", "-certpbe", "PBE-SHA1-3DES", "-macalg", "sha1"])
                var imported: CFArray?
                let options: [CFString: Any] = [kSecImportExportPassphrase: password,
                                                kSecImportExportKeychain: keychain]
                let result = SecPKCS12Import(try Data(contentsOf: p12) as CFData, options as CFDictionary, &imported)
                guard result == errSecSuccess,
                      let items = imported as? [[String: Any]],
                      let value = items.first?[kSecImportItemIdentity as String],
                      CFGetTypeID(value as CFTypeRef) == SecIdentityGetTypeID() else {
                    throw Failure.setup("PKCS12 import: \(result)")
                }
                let identity = value as! SecIdentity
                var privateKey: SecKey?
                guard SecIdentityCopyPrivateKey(identity, &privateKey) == errSecSuccess,
                      let privateKey else { throw Failure.setup("private key unavailable") }
                var signingError: Unmanaged<CFError>?
                guard SecKeyCreateSignature(privateKey, .rsaSignatureMessagePKCS1v15SHA256,
                    Data("native-test-key-check".utf8) as CFData, &signingError) != nil else {
                    let code = signingError.map { CFErrorGetCode($0.takeRetainedValue()) } ?? 0
                    throw Failure.setup("fixture key signing failed: \(code)")
                }
                var certificate: SecCertificate?
                guard SecIdentityCopyCertificate(identity, &certificate) == errSecSuccess,
                      let certificate else { throw Failure.setup("certificate missing") }
                self.directory = directory
                self.keychain = keychain
                self.identity = identity
                self.certificate = certificate
                self.pairing = WirelessPairing(boardID: "native-test-board", host: "localhost", port: 5181,
                    serverName: "localhost", serverCertificatePEM: try String(contentsOf: pem),
                    credential: String(repeating: "a", count: 64))
            } catch {
                SecKeychainDelete(keychain)
                try? FileManager.default.removeItem(at: directory)
                throw error
            }
        }
        deinit {
            SecKeychainDelete(keychain)
            try? FileManager.default.removeItem(at: directory)
        }
        private static func openssl(_ arguments: [String]) throws {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/openssl")
            process.arguments = arguments
            process.standardOutput = FileHandle.nullDevice
            process.standardError = FileHandle.nullDevice
            try process.run()
            process.waitUntilExit()
            guard process.terminationStatus == 0 else { throw Failure.setup("openssl fixture failed") }
        }
    }

    private final class Peer: @unchecked Sendable {
        private let queue = DispatchQueue(label: "test.native-microphone.peer")
        // All mailbox state is confined to the NWConnection callback queue.
        // Tests suspend rather than blocking the XCTest/main executor while
        // Network.framework and Security perform asynchronous setup.
        private var messages: [Wire.ControlMessage] = []
        private var closed = false
        private var waiter: (UUID, CheckedContinuation<Wire.ControlMessage, Error>)?
        private var closeWaiter: (UUID, CheckedContinuation<Bool, Never>)?
        let connection: NWConnection

        init(port: UInt16, fixture: IdentityFixture, serverName: String = "localhost") {
            let tls = NWProtocolTLS.Options()
            sec_protocol_options_set_tls_server_name(tls.securityProtocolOptions, serverName)
            sec_protocol_options_set_verify_block(tls.securityProtocolOptions, { _, trust, complete in
                let value = sec_trust_copy_ref(trust).takeRetainedValue()
                // Trust only this test certificate AND validate its hostname,
                // validity, and SSL policy. Never an unconditional accept block.
                let anchors = [fixture.certificate] as CFArray
                let policy = SecPolicyCreateSSL(true, serverName as CFString)
                let configured = SecTrustSetAnchorCertificates(value, anchors) == errSecSuccess
                    && SecTrustSetAnchorCertificatesOnly(value, true) == errSecSuccess
                    && SecTrustSetPolicies(value, policy) == errSecSuccess
                var trustError: CFError?
                let trusted = configured && SecTrustEvaluateWithError(value, &trustError)
                print("native-test tls-verify configured=\(configured) trusted=\(trusted) code=\(trustError.map { CFErrorGetCode($0) } ?? 0)")
                complete(trusted)
            }, queue)
            let parameters = NWParameters(tls: tls)
            let websocket = NWProtocolWebSocket.Options()
            websocket.autoReplyPing = true
            websocket.setSubprotocols([Wire.subprotocol])
            parameters.defaultProtocolStack.applicationProtocols.insert(websocket, at: 0)
            connection = NWConnection(host: "127.0.0.1", port: NWEndpoint.Port(rawValue: port)!, using: parameters)
            connection.stateUpdateHandler = { [weak self] state in
                guard let self else { return }
                switch state {
                case .ready:
                    print("native-test peer-ready")
                    self.receive()
                case let .failed(error):
                    print("native-test peer-failed \(error)")
                    self.markClosed()
                case let .waiting(error): print("native-test peer-waiting \(error)")
                case .preparing: print("native-test peer-preparing")
                case .cancelled: self.markClosed()
                default: break
                }
            }
            connection.start(queue: queue)
        }
        func cancel() { connection.cancel() }
        private func markClosed() {
            closed = true
            if let (_, continuation) = waiter {
                waiter = nil
                continuation.resume(throwing: Failure.closed)
            }
            if let (_, continuation) = closeWaiter {
                closeWaiter = nil
                continuation.resume(returning: true)
            }
        }
        private func receive() {
            connection.receiveMessage { [weak self] data, context, complete, error in
                guard let self else { return }
                guard error == nil,
                      let metadata = context?.protocolMetadata(definition: NWProtocolWebSocket.definition)
                        as? NWProtocolWebSocket.Metadata else { self.markClosed(); return }
                if metadata.opcode == .close { self.markClosed(); return }
                if complete, metadata.opcode == .text, let data, let message = try? Wire.decodeControl(data) {
                    if let (_, continuation) = self.waiter {
                        self.waiter = nil
                        continuation.resume(returning: message)
                    } else { self.messages.append(message) }
                }
                self.receive()
            }
        }
        func send(_ message: Wire.ControlMessage) throws { try send(Wire.encodeControl(message), opcode: .text) }
        func send(_ data: Data, opcode: NWProtocolWebSocket.Opcode) {
            let metadata = NWProtocolWebSocket.Metadata(opcode: opcode)
            let context = NWConnection.ContentContext(identifier: "test-wire", metadata: [metadata])
            connection.send(content: data, contentContext: context, isComplete: true,
                completion: .contentProcessed { [weak self] error in if error != nil { self?.markClosed() } })
        }
        func next(timeout: TimeInterval = 5) async throws -> Wire.ControlMessage {
            try await withCheckedThrowingContinuation { continuation in
                queue.async {
                    if !self.messages.isEmpty {
                        continuation.resume(returning: self.messages.removeFirst()); return
                    }
                    if self.closed { continuation.resume(throwing: Failure.closed); return }
                    precondition(self.waiter == nil)
                    let id = UUID()
                    self.waiter = (id, continuation)
                    self.queue.asyncAfter(deadline: .now() + timeout) {
                        guard let (pending, callback) = self.waiter, pending == id else { return }
                        self.waiter = nil
                        callback.resume(throwing: Failure.timeout)
                    }
                }
            }
        }
        func expectClosed(timeout: TimeInterval = 5) async -> Bool {
            await withCheckedContinuation { continuation in
                queue.async {
                    if self.closed { continuation.resume(returning: true); return }
                    precondition(self.closeWaiter == nil)
                    let id = UUID()
                    self.closeWaiter = (id, continuation)
                    self.queue.asyncAfter(deadline: .now() + timeout) {
                        guard let (pending, callback) = self.closeWaiter, pending == id else { return }
                        self.closeWaiter = nil
                        callback.resume(returning: false)
                    }
                }
            }
        }
    }

    private actor DrainGate {
        private var continuation: CheckedContinuation<Bool, Never>?
        private var released = false
        func wait() async -> Bool {
            if released { return true }
            return await withCheckedContinuation { continuation = $0 }
        }
        func release() {
            released = true
            continuation?.resume(returning: true)
            continuation = nil
        }
    }

    private func startServer(_ server: WirelessMicrophoneServer) async throws -> UInt16 {
        let ready = expectation(description: "real TLS listener ready")
        server.onStateChange = {
            print("native-test listener-state \($0)")
            if case .ready = $0 { ready.fulfill() }
        }
        try server.start()
        await fulfillment(of: [ready], timeout: 5)
        guard case let .ready(port) = server.state else { throw Failure.setup("listener not ready") }
        return port
    }
    private func authenticate(_ peer: Peer, fixture: IdentityFixture) async throws {
        try peer.send(.init(.hello, deviceID: fixture.pairing.boardID, credential: fixture.pairing.credential))
        let response = try await peer.next()
        XCTAssertEqual(response.type, .capabilities)
    }
    private func arm(_ peer: Peer, requestID: String) async throws -> Wire.ControlMessage {
        try peer.send(.init(.start, requestID: requestID, threadID: threadID, transport: "wifi"))
        let prepared = try await peer.next()
        XCTAssertEqual(prepared.type, .prepared)
        try peer.send(.init(.commit, sessionID: prepared.sessionID, generation: prepared.generation))
        let armed = try await peer.next()
        XCTAssertEqual(armed.type, .armed)
        return prepared
    }
    private func audio(_ peer: Peer, prepared: Wire.ControlMessage) throws {
        let session = try XCTUnwrap(prepared.sessionID.flatMap(UUID.init(uuidString:)))
        let data = try Wire.encodeAudioFrame(sessionID: session, sequence: 0, firstSample: 0,
            pcm: Data(repeating: 0, count: Wire.pcmBytesPerFrame))
        peer.send(data, opcode: .binary)
    }

    private func expect(_ type: Wire.ControlType, from peer: Peer) async throws {
        let message = try await peer.next()
        XCTAssertEqual(message.type, type)
    }
    private func expectNoMessage(from peer: Peer) async throws {
        do {
            _ = try await peer.next(timeout: 0.1)
            XCTFail("Unexpected control response")
        } catch Failure.timeout { /* expected: connected and silent */ }
    }

    func testRealTLSHandshakePCMDrainDuplicateStopAndFreshSession() async throws {
        let fixture = try IdentityFixture()
        let server = WirelessMicrophoneServer(configuration: .init(port: 0,
            pairingProvider: { fixture.pairing }, identityProvider: { fixture.identity }))
        let gate = DrainGate()
        let stopEntered = expectation(description: "one stop callback before acknowledgement")
        stopEntered.assertForOverFulfill = true
        server.onStart = { _, _ in true }
        server.onAudioFrame = { $0.sequence == 0 && $0.pcm.count == Wire.pcmBytesPerFrame }
        server.onStop = { _, _, _ in stopEntered.fulfill(); return await gate.wait() }
        let port = try await startServer(server)
        let peer = Peer(port: port, fixture: fixture)
        defer { peer.cancel(); server.stop(); Task { await gate.release() } }
        try await authenticate(peer, fixture: fixture)
        let first = try await arm(peer, requestID: "physical-1")
        try audio(peer, prepared: first)
        try await expect(.listening, from: peer)
        let stop = Wire.ControlMessage(.stop, sessionID: first.sessionID,
            generation: first.generation, finalSequence: 1)
        try peer.send(stop)
        await fulfillment(of: [stopEntered], timeout: 5)
        try peer.send(stop)
        try await expectNoMessage(from: peer)
        Task { await gate.release() }
        try await expect(.stopped, from: peer)
        // Reuse one authenticated real connection. A stale local cancellation
        // must not close the replacement session.
        let second = try await arm(peer, requestID: "physical-2")
        XCTAssertNotEqual(first.sessionID, second.sessionID)
        server.cancelActiveSession(sessionID: try XCTUnwrap(first.sessionID.flatMap(UUID.init(uuidString:))))
        try audio(peer, prepared: second)
        try await expect(.listening, from: peer)
    }

    func testRealTLSNameVerificationSinkRejectionAndReconnect() async throws {
        let fixture = try IdentityFixture()
        let server = WirelessMicrophoneServer(configuration: .init(port: 0,
            pairingProvider: { fixture.pairing }, identityProvider: { fixture.identity }))
        let failed = expectation(description: "rejected sink terminates exactly its receiver")
        server.onStart = { _, _ in true }
        server.onAudioFrame = { _ in false }
        server.onSessionFailure = { _, _, _ in failed.fulfill() }
        let port = try await startServer(server)
        defer { server.stop() }
        let wrongName = Peer(port: port, fixture: fixture, serverName: "not-localhost.invalid")
        defer { wrongName.cancel() }
        let nameRejected = await wrongName.expectClosed()
        XCTAssertTrue(nameRejected)
        // A rejected handshake may finish its server-side cancellation on the
        // next queue turn. A bounded retry here opens only TLS, never a take.
        var peer: Peer?
        for _ in 0..<5 {
            let candidate = Peer(port: port, fixture: fixture)
            do { try await authenticate(candidate, fixture: fixture); peer = candidate; break }
            catch { candidate.cancel() }
        }
        let current = try XCTUnwrap(peer)
        defer { current.cancel() }
        let prepared = try await arm(current, requestID: "rejected-audio")
        try audio(current, prepared: prepared)
        let sinkRejected = await current.expectClosed()
        XCTAssertTrue(sinkRejected)
        await fulfillment(of: [failed], timeout: 5)
        if let message = try? await current.next(timeout: 0.1) { XCTAssertNotEqual(message.type, .listening) }
        let replacement = Peer(port: port, fixture: fixture)
        defer { replacement.cancel() }
        try await authenticate(replacement, fixture: fixture)
        // No automatic recording on reconnection; an explicit new start is
        // needed, and no audio is sent by authenticate().
        try await expectNoMessage(from: replacement)
    }
}
