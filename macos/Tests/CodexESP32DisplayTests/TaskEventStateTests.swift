import XCTest
@testable import CodexESP32Display

final class TaskEventStateTests: XCTestCase {
    let a = "01a06f9d-249c-7f43-b019-95324c366b8c"
    let b = "01a070ea-33cd-7642-89f9-86181a0c85ad"
    let client = "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa"
    let other = "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb"
    let now = Date(timeIntervalSince1970: 100)

    func event(_ id: String, _ following: Any, source: String? = nil, host: String = "local", version: Int = 1) throws -> Data {
        try JSONSerialization.data(withJSONObject: ["type": "broadcast", "method": "thread-stream-following-changed",
            "version": version, "sourceClientId": source ?? client,
            "params": ["conversationId": id, "hostId": host, "following": following]])
    }
    func result(_ state: TaskEventState) -> FocusedTaskSelection { state.result(at: now.addingTimeInterval(1)).0 }

    func testSwitchAndNonTaskPageClearExactIdentity() throws {
        var state = TaskEventState(at: now)
        try state.apply(event(a, true), at: now)
        XCTAssertEqual(state.result(at: now).0.status, .unavailable)
        XCTAssertEqual(result(state).threadId, a)
        try state.apply(event(a, false), at: now)
        try state.apply(event(b, true), at: now)
        XCTAssertEqual(result(state).threadId, b)
        try state.apply(event(b, false), at: now)
        XCTAssertEqual(result(state).status, .noTask)
        XCTAssertNil(result(state).threadId)
    }

    func testAmbiguityAcrossClientsAndWithinOneClient() throws {
        var state = TaskEventState(at: now)
        try state.apply(event(a, true), at: now)
        try state.apply(event(a, true, source: other), at: now)
        XCTAssertEqual(state.candidateTasks.map(\.threadId), [a, a])
        XCTAssertEqual(state.candidateTasks.map(\.clientId), [client, other])
        XCTAssertEqual(state.result(at: now.addingTimeInterval(1)).1, "events-ambiguous")
        try state.apply(event(a, false, source: other), at: now)
        XCTAssertEqual(state.candidateTasks.count, 1)
        XCTAssertEqual(result(state).threadId, a)
        try state.apply(event(b, true), at: now)
        XCTAssertEqual(state.candidateTasks.map(\.threadId), [a, b])
        XCTAssertNil(result(state).threadId)
    }

    func testFalseFromAnotherClientCannotClearThePresentedTask() throws {
        var state = TaskEventState(at: now)
        try state.apply(event(a, true), at: now)
        try state.apply(event(a, false, source: other), at: now)
        XCTAssertEqual(result(state).threadId, a)
        // Duplicate announcements are idempotent.
        try state.apply(event(a, true), at: now)
        XCTAssertEqual(state.candidateCount, 1)
    }

    func testSourceDisconnectRemovesOnlyItsOwnTargets() throws {
        var state = TaskEventState(at: now)
        try state.apply(event(a, true), at: now)
        try state.apply(event(b, true, source: other), at: now)
        let disconnect = try JSONSerialization.data(withJSONObject: ["type": "broadcast", "method": "client-status-changed",
            "version": 0, "params": ["clientId": other, "status": "disconnected"]])
        try state.apply(disconnect, at: now)
        XCTAssertEqual(result(state).threadId, a)
        XCTAssertEqual(result(TaskEventState(at: now)).status, .noTask)
    }

    func testRemoteHostNeverResolvesAgainstLocalTasks() throws {
        var state = TaskEventState(at: now)
        try state.apply(event(a, true, host: "remote-host"), at: now)
        XCTAssertEqual(result(state).status, .unsupportedHost)
        XCTAssertNil(result(state).threadId)
        try state.apply(event(a, true), at: now)
        XCTAssertEqual(result(state).status, .unavailable)
    }

    func testRejectsIncompatibleAndMalformedSelectionEvents() throws {
        for data in [try event(a, true, version: 2), try event("temporary", true),
                     try event(a, 1), try event(a, "true"), try event(a, true, source: "bad-client"),
                     try event(a, true, host: "")] {
            var state = TaskEventState(at: now)
            XCTAssertThrowsError(try state.apply(data, at: now))
        }
    }

    func testOtherEventsCannotSelectATask() throws {
        var state = TaskEventState(at: now)
        let data = try JSONSerialization.data(withJSONObject: ["type": "broadcast", "method": "thread-read-state-changed",
            "params": ["conversationId": a, "following": true]])
        try state.apply(data, at: now)
        XCTAssertEqual(result(state).status, .noTask)
    }

    func testFragmentedAndCoalescedFrames() throws {
        let first = try TaskEventFrames.encode(["type": "test", "value": "é"])
        let second = try TaskEventFrames.encode(["type": "other"])
        var parser = TaskEventFrames()
        XCTAssertTrue(try parser.append(first.prefix(2)).isEmpty)
        XCTAssertTrue(try parser.append(first.dropFirst(2).prefix(3)).isEmpty)
        let parsed = try parser.append(first.dropFirst(5) + second)
        XCTAssertEqual(parsed.count, 2)
        XCTAssertEqual((try JSONSerialization.jsonObject(with: parsed[0]) as? [String: String])?["value"], "é")
        var bytewise = TaskEventFrames()
        var output: [Data] = []
        for byte in first { output += try bytewise.append(Data([byte])) }
        XCTAssertEqual(output, [Data(first.dropFirst(4))])
    }

    func testOversizedAndEmptyFramesFailBeforePayloadAllocation() {
        for header: [UInt8] in [[0, 0, 0, 0], [255, 255, 255, 127]] {
            var parser = TaskEventFrames()
            XCTAssertThrowsError(try parser.append(Data(header)))
        }
    }
}
