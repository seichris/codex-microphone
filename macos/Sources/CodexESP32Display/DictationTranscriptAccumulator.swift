import Foundation

/// Recognition callbacks are replacement hypotheses, not appended text.
/// Preserve separate utterances only when non-overlapping audio times prove a
/// boundary. Metadata presence and textual similarity do not prove finality.
struct DictationTranscriptAccumulator {
    private struct Hypothesis {
        var text: String
        var audioRange: Range<TimeInterval>
    }
    private var timed: [Hypothesis] = []
    private var untimed: String?

    var text: String {
        // Missing timing cannot establish whether a result is cumulative.
        // Display it as a replacement; retain timed snapshots for a later
        // result whose audio range can resolve the boundary.
        if let untimed { return untimed }
        return timed.map(\.text).joined(separator: " ")
    }

    @discardableResult
    mutating func update(_ rawText: String, audioRange: Range<TimeInterval>? = nil) -> String {
        let incoming = rawText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !incoming.isEmpty else { return text }
        guard let range = audioRange, range.lowerBound.isFinite,
              range.upperBound.isFinite, range.lowerBound >= 0,
              range.upperBound > range.lowerBound else {
            untimed = incoming
            return text
        }
        if let last = timed.last, range.upperBound <= last.audioRange.lowerBound {
            return text // A delayed older utterance cannot replace the latest one.
        }
        untimed = nil
        // Replace overlapping hypotheses, including a cumulative result which
        // revises several earlier utterances. Equal words at disjoint times are
        // actual repetition and must remain intact.
        if let overlap = timed.firstIndex(where: { $0.audioRange.upperBound > range.lowerBound }) {
            timed.removeSubrange(overlap...)
        }
        timed.append(Hypothesis(text: incoming, audioRange: range))
        return text
    }

    mutating func reset() {
        timed.removeAll(keepingCapacity: false)
        untimed = nil
    }
}
