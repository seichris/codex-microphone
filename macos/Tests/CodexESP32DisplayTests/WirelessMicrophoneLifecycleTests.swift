import Foundation
import XCTest
@testable import CodexESP32Display

final class WirelessMicrophoneLifecycleTests: XCTestCase {
    private let credential = String(repeating: "a", count: 64)
    private let threadID = "01a06f9d-249c-7f43-b019-95324c366b8c"

    private func startMessage(_ id: String = "gesture-1") -> WirelessMicrophoneProtocol.ControlMessage {
        .init(.start, requestID: id, threadID: threadID, transport: "wifi")
    }

    private func preparedSession() throws -> WirelessMicrophoneSession {
        var session = WirelessMicrophoneSession(expectedCredential: credential)
        _ = try session.authenticate(.init(.hello, deviceID: "test-board", credential: credential))
        _ = try session.start(startMessage())
        return session
    }

    private func command(_ type: WirelessMicrophoneProtocol.ControlType, for session: WirelessMicrophoneSession) -> WirelessMicrophoneProtocol.ControlMessage {
        .init(type, sessionID: session.sessionID?.uuidString, generation: session.generation,
              finalSequence: 0, errorCode: "user_canceled")
    }

    func testCanceledPreparedSessionStillRequiresReceiverCleanup() throws {
        var session = try preparedSession()
        XCTAssertTrue(session.phase.requiresReceiverCancellation)
        try session.cancel(command(.cancel, for: session))
        XCTAssertEqual(session.phase, .canceled)
        XCTAssertTrue(session.phase.requiresReceiverCancellation)
        XCTAssertNotNil(session.snapshot.sessionID)
        XCTAssertEqual(session.snapshot.threadID, threadID)
    }

    func testCanceledArmedSessionCannotBeRearmedOrReplayed() throws {
        var session = try preparedSession()
        _ = try session.commit(command(.commit, for: session))
        try session.cancel(command(.cancel, for: session))
        XCTAssertTrue(session.phase.requiresReceiverCancellation)
        XCTAssertThrowsError(try session.commit(command(.commit, for: session)))
        XCTAssertThrowsError(try session.start(startMessage()))
        XCTAssertEqual(session.phase, .canceled)
    }

    func testNormalStopAndLateCancelDoNotReportInterruption() throws {
        var session = try preparedSession()
        _ = try session.commit(command(.commit, for: session))
        _ = try session.stop(command(.stop, for: session))
        XCTAssertFalse(session.phase.requiresReceiverCancellation)
        try session.cancel(command(.cancel, for: session))
        XCTAssertEqual(session.phase, .stopped)
        XCTAssertFalse(session.phase.requiresReceiverCancellation)
    }

    func testIdleAndAuthenticatedConnectionsNeedNoReceiverCleanup() throws {
        var session = WirelessMicrophoneSession(expectedCredential: credential)
        XCTAssertFalse(session.phase.requiresReceiverCancellation)
        _ = try session.authenticate(.init(.hello, deviceID: "test-board", credential: credential))
        XCTAssertFalse(session.phase.requiresReceiverCancellation)
    }

    func testCompletedStartReplayDoesNotCreateAnotherRecording() throws {
        var session = try preparedSession()
        _ = try session.commit(command(.commit, for: session))
        _ = try session.stop(command(.stop, for: session))
        let stopped = session.snapshot
        XCTAssertThrowsError(try session.start(startMessage())) { error in
            XCTAssertEqual(error as? WirelessMicrophoneSession.Error, .requestIDReused)
        }
        XCTAssertEqual(session.snapshot, stopped)
    }

    func testNextPhysicalGestureStillCreatesFreshSession() throws {
        var session = try preparedSession()
        let previous = session.snapshot
        _ = try session.commit(command(.commit, for: session))
        _ = try session.stop(command(.stop, for: session))
        _ = try session.start(startMessage("gesture-2"))
        XCTAssertNotEqual(session.sessionID, previous.sessionID)
        XCTAssertEqual(session.generation, previous.generation + 1)
        XCTAssertEqual(session.phase, .prepared)
    }

    func testDuplicateLiveStartDoesNotResetAcceptedAudio() throws {
        var session = try preparedSession()
        _ = try session.commit(command(.commit, for: session))
        _ = try session.acceptAudio(.init(sessionID: try XCTUnwrap(session.sessionID),
            sequence: 0, firstSample: 0,
            pcm: Data(repeating: 0, count: WirelessMicrophoneProtocol.pcmBytesPerFrame)))
        let before = session.snapshot
        _ = try session.start(startMessage())
        XCTAssertEqual(session.snapshot, before)
        XCTAssertTrue(session.phase.requiresReceiverCancellation)
    }

    func testStaleCancelCannotChangeCurrentSession() throws {
        var session = try preparedSession()
        let before = session.snapshot
        XCTAssertThrowsError(try session.cancel(.init(.cancel, sessionID: UUID().uuidString,
            generation: session.generation, errorCode: "user_canceled")))
        XCTAssertEqual(session.snapshot, before)
    }
}
