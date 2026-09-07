import Foundation
import XCTest
@testable import CodexESP32Display

final class WirelessMicrophoneServerTests: XCTestCase {
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
