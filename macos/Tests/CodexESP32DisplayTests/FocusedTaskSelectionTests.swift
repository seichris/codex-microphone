import XCTest
@testable import CodexESP32Display

final class FocusedTaskSelectionTests: XCTestCase {
    let id = "01a070ea-33cd-7642-89f9-86181a0c85ad"

    func testExactMainDocumentRoute() {
        let selection = FocusedTaskSelection.document("app://-/local/\(id)?hostId=local")
        XCTAssertEqual(selection.status, .confirmed)
        XCTAssertEqual(selection.threadId, id)
        XCTAssertEqual(selection.hostId, "local")
        XCTAssertEqual(FocusedTaskSelection.document("app://-/local/\(id)").status, .confirmed)
    }

    func testForeignAndAmbiguousSourcesNeverConfirm() {
        for source in ["https://example.com/local/\(id)", "codex://threads/\(id)",
                       "app://fs/local/\(id)", "app://user@-/local/\(id)",
                       "app://-/local/\(id)?hostId=local&hostId=remote",
                       "app://-/local/\(id)?hostId", "app://-/local//\(id)",
                       "app://-/local/temporary-client-id", "app://-/local/\(id)#other"] {
            XCTAssertEqual(FocusedTaskSelection.document(source).status, .unavailable, source)
        }
        let remote = FocusedTaskSelection.document("app://-/local/\(id)?hostId=remote")
        XCTAssertEqual(remote.status, .unsupportedHost)
        XCTAssertNil(remote.threadId)
    }

    func testBootstrapDocumentCannotConfirmItsStaleInitialRoute() {
        for source in ["app://-/index.html", "app://-/index.html?initialRoute=/local/\(id)",
                       "app://-/index.html?initialRoute=%2Flocal%2F\(id)%3FhostId%3Dlocal"] {
            let selection = FocusedTaskSelection.document(source)
            XCTAssertEqual(selection.status, .unavailable)
            XCTAssertNil(selection.threadId)
        }
    }

    func testOperationalDiagnosticsDoNotIncludeSelectionContent() throws {
        var diagnostic = FocusedTaskDiagnostic(reason: "shell-document")
        diagnostic.axErrors["AXDocument"] = -25205
        diagnostic.visited = 8
        diagnostic.webAreas = 1
        let data = try JSONEncoder().encode(diagnostic)
        let payload = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
        XCTAssertEqual(Set(payload.keys), Set(["reason", "appCount", "processID", "trusted", "axErrors", "visited", "webAreas", "candidates", "uniqueCandidates", "candidateTasks", "elapsedMilliseconds"]))
        XCTAssertFalse(diagnostic.message.contains("permission"))
        XCTAssertNotEqual(diagnostic.message, FocusedTaskDiagnostic(reason: "permission-needed").message)
    }

    func testNonTaskNavigationClearsIdentity() {
        for source in ["app://-/", "app://-/local/new", "app://-/settings", "app://-/work/conversation/\(id)"] {
            let selection = FocusedTaskSelection.document(source)
            XCTAssertEqual(selection.status, .noTask)
            XCTAssertNil(selection.threadId)
        }
    }

    func testConfirmationExpiresWithoutFreshObservation() {
        let now = Date()
        let selection = FocusedTaskSelection.document("app://-/local/\(id)", at: now)
        XCTAssertEqual(selection.fresh(at: now.addingTimeInterval(2)).status, .confirmed)
        XCTAssertEqual(selection.fresh(at: now.addingTimeInterval(4)).status, .unavailable)
        XCTAssertEqual(selection.fresh(at: now.addingTimeInterval(-1)).status, .unavailable)
    }

    func testSelectionDoesNotInheritAnotherTasksRecordingState() {
        let selected = FocusedTaskSelection.document("app://-/local/\(id)")
        XCTAssertEqual(selected.voiceState(for: id, state: "listening"), "listening")
        XCTAssertEqual(selected.voiceState(for: "01a070ea-33cd-7642-89f9-86181a0c85ae", state: "listening"), "unknown")
        XCTAssertEqual(selected.voiceState(for: nil, state: "error"), "unknown")
        XCTAssertEqual(FocusedTaskSelection.unavailable().voiceState(for: id, state: "listening"), "unknown")
    }
}
