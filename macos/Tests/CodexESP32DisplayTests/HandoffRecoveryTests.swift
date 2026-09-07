import XCTest
@testable import CodexESP32Display

final class HandoffRecoveryTests: XCTestCase {
    @MainActor
    func testLocalLinkAcceptanceKeepsOriginalTargetRecoveryWithoutReplay() async throws {
        let target = "01a06f9d-249c-7f43-b019-95324c366b8c"
        var session = DictationSession()
        let id = try session.begin(threadId: target)
        session.recording(id)
        let opened = expectation(description: "one URL open, not a composer receipt")
        opened.assertForOverFulfill = true
        let model = DictationModel(session: session) { _ in opened.fulfill() }
        model.handle(id: id, event: .finishing)
        model.handle(id: id, event: .transcript("Retain this recovery text", final: true))
        await fulfillment(of: [opened], timeout: 2)
        model.handle(id: id, event: .transcript("Obsolete revision", final: true))
        XCTAssertEqual(model.lastAttemptedHandoff?.threadID, target)
        XCTAssertEqual(model.lastAttemptedHandoff?.text, "Retain this recovery text")
        XCTAssertEqual(model.session.phase, .idle)
        model.clearHandoffRecovery()
        XCTAssertNil(model.lastAttemptedHandoff)
    }
}
