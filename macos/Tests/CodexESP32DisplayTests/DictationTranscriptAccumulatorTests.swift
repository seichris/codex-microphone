import XCTest
@testable import CodexESP32Display

final class DictationTranscriptAccumulatorTests: XCTestCase {
    func testDisjointGuessesAreRevisionsWithoutTimingEvidence() {
        var value = DictationTranscriptAccumulator()
        for guess in ["What the PR", "What's the PRS", "No the PR", "Merge the PRs"] {
            XCTAssertEqual(value.update(guess), guess)
        }
    }

    func testShorterAndCompletelyDifferentTimedRevisionsReplaceOldGuess() {
        var value = DictationTranscriptAccumulator()
        _ = value.update("What the PR What's the PRS", audioRange: 0.2..<1.9)
        XCTAssertEqual(value.update("Merge the PRs", audioRange: 0.3..<1.8), "Merge the PRs")
        XCTAssertEqual(value.update("Merge", audioRange: 0.3..<1.0), "Merge")
    }

    func testSeparateUtterancesSurvivePauseWithoutTextDeduplication() {
        var value = DictationTranscriptAccumulator()
        _ = value.update("Yes", audioRange: 0.2..<0.8)
        _ = value.update("Yes", audioRange: 2.0..<2.7)
        XCTAssertEqual(value.text, "Yes Yes")
        XCTAssertEqual(value.update("Yes!", audioRange: 2.0..<2.7), "Yes Yes!")
    }

    func testCumulativeResultReplacesOverlappingUtterances() {
        var value = DictationTranscriptAccumulator()
        _ = value.update("First phrase", audioRange: 0.1..<1.0)
        _ = value.update("second phrase", audioRange: 2.0..<3.0)
        XCTAssertEqual(value.update("First phrase, second phrase.", audioRange: 0.1..<3.0),
                       "First phrase, second phrase.")
    }

    func testUntimedRevisionDoesNotDestroyPriorTimedBoundary() {
        var value = DictationTranscriptAccumulator()
        _ = value.update("First phrase", audioRange: 0.1..<1.0)
        _ = value.update("second guess")
        XCTAssertEqual(value.update("Second phrase", audioRange: 2.0..<3.0), "First phrase Second phrase")
    }

    func testUntimedCorrectionWaitsForTimingBeforeJoining() {
        var value = DictationTranscriptAccumulator()
        _ = value.update("First phrase", audioRange: 0.1..<1.0)
        _ = value.update("wrong guess", audioRange: 2.0..<3.0)
        XCTAssertEqual(value.update("Right phrase"), "Right phrase")
        XCTAssertEqual(value.update("Right phrase", audioRange: 2.0..<3.0), "First phrase Right phrase")
    }

    func testUntimedCumulativeResultDoesNotDuplicatePriorUtterances() {
        var value = DictationTranscriptAccumulator()
        _ = value.update("First", audioRange: 0.1..<1.0)
        _ = value.update("Second", audioRange: 2.0..<3.0)
        XCTAssertEqual(value.update("First Second revised"), "First Second revised")
    }

    func testStaleOlderResultDoesNotEraseCurrentUtterance() {
        var value = DictationTranscriptAccumulator()
        _ = value.update("First", audioRange: 0.1..<1.0)
        _ = value.update("Second", audioRange: 2.0..<3.0)
        XCTAssertEqual(value.update("Stale", audioRange: 0.1..<1.0), "First Second")
    }

    func testEmptyAndUnavailableTimingsDoNotCommitGuesses() {
        var value = DictationTranscriptAccumulator()
        _ = value.update("Wrong", audioRange: 0..<0)
        XCTAssertEqual(value.update("Right"), "Right")
        XCTAssertEqual(value.update(" \n"), "Right")
        value.reset()
        XCTAssertEqual(value.text, "")
        XCTAssertEqual(value.update("New take"), "New take")
    }
}
