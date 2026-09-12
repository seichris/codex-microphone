import XCTest
@testable import CodexESP32Display

final class FluidVoiceModelTests: XCTestCase {
    private let target = "01a06f9d-249c-7f43-b019-95324c366b8c"

    @MainActor
    func testReviewMustBeExplicitAndKeepsOriginalTaskOnce() async throws {
        var session = DictationSession()
        let id = try session.begin(threadId: target)
        session.recording(id)
        var opened: [URL] = []
        let didOpen = expectation(description: "explicit draft open")
        didOpen.assertForOverFulfill = true
        let model = DictationModel(session: session, sessionEngine: .fluidVoice) { url in
            opened.append(url); didOpen.fulfill()
        }
        model.handle(id: id, event: .finishing)
        model.handle(id: id, event: .transcript("Review 世界", final: true))
        XCTAssertTrue(opened.isEmpty)
        XCTAssertEqual(model.session.phase, .draft)
        model.draftText = "Edited 世界"
        model.handle(id: id, event: .transcript("Late text", final: true))
        XCTAssertEqual(model.draftText, "Edited 世界")
        model.openDraft()
        model.openDraft()
        await fulfillment(of: [didOpen], timeout: 2)
        XCTAssertEqual(opened, [try XCTUnwrap(DictationDraftLink.url(threadId: target, text: "Edited 世界"))])
        XCTAssertEqual(model.lastAttemptedHandoff?.threadID, target)
        XCTAssertEqual(model.lastAttemptedHandoff?.text, "Edited 世界")
        XCTAssertEqual(model.session.phase, .idle)
    }

    @MainActor
    func testCancelledSessionRejectsQueuedFinalAndRecordingEvents() throws {
        var session = DictationSession()
        let id = try session.begin(threadId: target)
        session.recording(id)
        let model = DictationModel(session: session, sessionEngine: .fluidVoice) { _ in XCTFail("Cancelled take") }
        model.cancelRecording()
        model.handle(id: id, event: .transcript("Too late", final: true))
        model.handle(id: id, event: .recording)
        XCTAssertEqual(model.session.phase, .error)
        XCTAssertTrue(model.draftText.isEmpty)
        model.discardDraft()
        XCTAssertEqual(model.session.phase, .idle)
    }

    @MainActor
    func testFailedExplicitHandoffPreservesEditableDraft() async throws {
        var session = DictationSession()
        let id = try session.begin(threadId: target)
        session.recording(id)
        let attempted = expectation(description: "handoff attempted")
        let model = DictationModel(session: session, sessionEngine: .fluidVoice) { _ in
            attempted.fulfill()
            throw DictationError.message("fixture failure")
        }
        model.handle(id: id, event: .transcript("Preserve me", final: true))
        model.openDraft()
        await fulfillment(of: [attempted], timeout: 2)
        // Wait for the handoff task's cleanup, not for a new system interaction.
        while model.isOpeningDraft { await Task.yield() }
        XCTAssertEqual(model.draftText, "Preserve me")
        XCTAssertEqual(model.session.threadId, target)
        XCTAssertEqual(model.session.phase, .draft)
        XCTAssertNotNil(model.handoffMessage)
    }
}
