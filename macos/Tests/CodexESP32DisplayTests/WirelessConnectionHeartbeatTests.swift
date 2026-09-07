import Foundation
import XCTest
@testable import CodexESP32Display

final class WirelessConnectionHeartbeatTests: XCTestCase {
    private final class ReplyBox: @unchecked Sendable {
        private let lock = NSLock()
        private var value: WirelessConnectionHeartbeat.Reply?
        func set(_ reply: @escaping WirelessConnectionHeartbeat.Reply) {
            lock.lock(); defer { lock.unlock() }; value = reply
        }
        func reply(_ alive: Bool) {
            lock.lock(); let callback = value; lock.unlock(); callback?(alive)
        }
    }

    func testSilentPeerExpiresAndLatePongCannotReviveIt() {
        let queue = DispatchQueue(label: "test.heartbeat.silent")
        let replies = ReplyBox()
        let failed = expectation(description: "dead peer releases its slot exactly once")
        failed.assertForOverFulfill = true
        let heartbeat = WirelessConnectionHeartbeat(queue: queue, interval: 0.01, timeout: 0.03,
            ping: { replies.set($0) }, failure: { failed.fulfill() })
        heartbeat.start()
        wait(for: [failed], timeout: 1)
        replies.reply(true)
        replies.reply(false)
        let settled = expectation(description: "late callbacks settled")
        queue.asyncAfter(deadline: .now() + 0.06) { settled.fulfill() }
        wait(for: [settled], timeout: 1)
        heartbeat.stop()
    }

    func testResponsivePeerSurvivesMultipleDeadlines() {
        let queue = DispatchQueue(label: "test.heartbeat.alive")
        let probes = expectation(description: "healthy peer keeps its slot")
        probes.expectedFulfillmentCount = 3
        let failed = expectation(description: "healthy peer must not fail")
        failed.isInverted = true
        let heartbeat = WirelessConnectionHeartbeat(queue: queue, interval: 0.01, timeout: 0.03,
            ping: { reply in probes.fulfill(); reply(true) }, failure: { failed.fulfill() })
        heartbeat.start()
        wait(for: [probes], timeout: 1)
        heartbeat.stop()
        wait(for: [failed], timeout: 0.06)
    }

    func testStoppingWithPendingPingIgnoresLateReplyAndDeadline() {
        let queue = DispatchQueue(label: "test.heartbeat.stopped")
        let replies = ReplyBox()
        let probe = expectation(description: "one pending ping")
        probe.assertForOverFulfill = true
        let failed = expectation(description: "stopped context must not affect replacement")
        failed.isInverted = true
        let heartbeat = WirelessConnectionHeartbeat(queue: queue, interval: 0.01, timeout: 0.03,
            ping: { replies.set($0); probe.fulfill() }, failure: { failed.fulfill() })
        heartbeat.start()
        wait(for: [probe], timeout: 1)
        heartbeat.stop()
        replies.reply(true)
        replies.reply(false)
        wait(for: [failed], timeout: 0.08)
    }
}
