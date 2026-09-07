import Foundation
import XCTest
@testable import CodexESP32Display

final class WirelessMicrophoneServerTests: XCTestCase {
    func testCertificateParserRequiresOneRealCertificate() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let certificate = directory.appendingPathComponent("certificate.pem")
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/openssl")
        process.arguments = ["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                             "-subj", "/CN=wireless.test", "-keyout", directory.appendingPathComponent("key.pem").path,
                             "-out", certificate.path]
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        try process.run()
        process.waitUntilExit()
        XCTAssertEqual(process.terminationStatus, 0)
        let pem = try String(contentsOf: certificate)
        let der = try XCTUnwrap(WirelessPairingStore.certificateDER(pem))
        XCTAssertEqual(WirelessPairingStore.certificateDER(pem.replacingOccurrences(of: "\n", with: "\r\n")), der)
        XCTAssertNil(WirelessPairingStore.certificateDER(pem + pem))
        XCTAssertNil(WirelessPairingStore.certificateDER("-----BEGIN CERTIFICATE-----\nYQ==\n-----END CERTIFICATE-----"))
        XCTAssertNil(WirelessPairingStore.certificateDER("unrelated data"))
    }

    func testStateCallbackCanReadReadinessOnServerQueue() {
        let server = WirelessMicrophoneServer()
        let notified = expectation(description: "stop callback can read state without deadlocking")
        server.onStateChange = { state in
            XCTAssertEqual(state, .stopped)
            XCTAssertEqual(server.state, .stopped)
            XCTAssertFalse(server.isReady)
            notified.fulfill()
        }
        defer { server.onStateChange = nil }
        server.stop()
        wait(for: [notified], timeout: 2)
    }

    func testConcurrentStopAndReadinessQueriesAreSerialized() {
        let server = WirelessMicrophoneServer()
        DispatchQueue.concurrentPerform(iterations: 100) { _ in
            server.stop()
            XCTAssertFalse(server.isReady)
            XCTAssertEqual(server.state, .stopped)
        }
        XCTAssertEqual(server.state, .stopped)
    }
}
