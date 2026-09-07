import Foundation
import XCTest
@testable import CodexESP32Display

final class WirelessAudioIngressTests: XCTestCase {
    func testBoundedAdmissionAndStopBarrier() {
        let queue = DispatchQueue(label: "test.ingress.drain")
        let ingress = WirelessAudioIngress(queue: queue, capacity: 2)
        let id = UUID()
        let entered = expectation(description: "consumer blocked")
        let released = DispatchSemaphore(value: 0)
        let second = expectation(description: "second frame before barrier")
        let drained = expectation(description: "barrier after frames")
        ingress.open(sessionID: id)
        XCTAssertTrue(ingress.enqueue(sessionID: id) {
            entered.fulfill()
            _ = released.wait(timeout: .now() + 2)
        })
        wait(for: [entered], timeout: 1)
        XCTAssertTrue(ingress.enqueue(sessionID: id) { second.fulfill() })
        XCTAssertFalse(ingress.enqueue(sessionID: id) { XCTFail("overflow was admitted") })
        XCTAssertTrue(ingress.close(sessionID: id) { drained.fulfill() })
        XCTAssertFalse(ingress.enqueue(sessionID: id) { XCTFail("post-stop frame") })
        released.signal()
        wait(for: [second, drained], timeout: 1, enforceOrder: true)
    }

    func testOldCloseDoesNotRevokeSuccessor() {
        let queue = DispatchQueue(label: "test.ingress.generation")
        let ingress = WirelessAudioIngress(queue: queue)
        let old = UUID(), next = UUID()
        ingress.open(sessionID: old)
        XCTAssertTrue(ingress.close(sessionID: old))
        ingress.open(sessionID: next)
        XCTAssertFalse(ingress.close(sessionID: old))
        XCTAssertFalse(ingress.enqueue(sessionID: old) { XCTFail("stale frame") })
        let consumed = expectation(description: "successor accepted")
        XCTAssertTrue(ingress.enqueue(sessionID: next) { consumed.fulfill() })
        wait(for: [consumed], timeout: 1)
    }

    func testSuccessorCannotResetOutstandingMemoryBudget() {
        let queue = DispatchQueue(label: "test.ingress.bound")
        let ingress = WirelessAudioIngress(queue: queue, capacity: 1)
        let old = UUID(), next = UUID()
        let blocked = expectation(description: "old block retained")
        let release = DispatchSemaphore(value: 0)
        ingress.open(sessionID: old)
        XCTAssertTrue(ingress.enqueue(sessionID: old) {
            blocked.fulfill()
            _ = release.wait(timeout: .now() + 2)
        })
        wait(for: [blocked], timeout: 1)
        ingress.open(sessionID: next)
        XCTAssertFalse(ingress.enqueue(sessionID: next) { XCTFail("budget reset") })
        release.signal()
        queue.sync {}
        let accepted = expectation(description: "released slot reusable")
        XCTAssertTrue(ingress.enqueue(sessionID: next) { accepted.fulfill() })
        wait(for: [accepted], timeout: 1)
    }

    func testConcurrentCloseNeverOvertakesAdmittedWork() {
        let queue = DispatchQueue(label: "test.ingress.race")
        let ingress = WirelessAudioIngress(queue: queue)
        for _ in 0..<100 {
            let id = UUID()
            ingress.open(sessionID: id)
            let group = DispatchGroup()
            group.enter()
            DispatchQueue.global().async {
                ingress.enqueue(sessionID: id) {}
                group.leave()
            }
            XCTAssertTrue(ingress.close(sessionID: id))
            group.wait()
            XCTAssertFalse(ingress.enqueue(sessionID: id) { XCTFail("late admission") })
            queue.sync {}
        }
    }
}
