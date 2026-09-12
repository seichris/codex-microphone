import Foundation
import XCTest
@testable import CodexESP32Display

final class FluidVoiceRecorderTests: XCTestCase {
    private actor Transcriber: FluidVoiceTranscribing {
        var recordings: [Data] = []
        func transcribe(pcm: Data, port: Int) async throws -> String {
            recordings.append(pcm)
            return "Complete recording"
        }
    }

    func testWirelessDrainTranscribesEveryFrameOnceWithoutSpeechPermissions() async throws {
        let client = Transcriber()
        let recorder = DictationRecorder(fluidVoice: client)
        let id = UUID(), wirelessID = UUID()
        let prepared = expectation(description: "receiver prepared")
        let final = expectation(description: "single final result")
        final.assertForOverFulfill = true
        try await recorder.start(id: id, transport: .wifi, wirelessSessionID: wirelessID, engine: .fluidVoice) { event in
            switch event {
            case .prepared: prepared.fulfill()
            case let .transcript(text, isFinal):
                XCTAssertEqual(text, "Complete recording"); XCTAssertTrue(isFinal); final.fulfill()
            case let .failed(message): XCTFail(message)
            default: break
            }
        }
        await fulfillment(of: [prepared], timeout: 2)
        let pcm = Data(repeating: 0x10, count: WirelessMicrophoneProtocol.pcmBytesPerFrame)
        for sequence in 0..<5 {
            XCTAssertTrue(recorder.appendWirelessFrame(.init(sessionID: wirelessID, sequence: UInt32(sequence),
                firstSample: UInt64(sequence * 960), pcm: pcm)))
        }
        let drained = await recorder.finishWireless(sessionID: wirelessID)
        XCTAssertTrue(drained)
        XCTAssertFalse(recorder.appendWirelessFrame(.init(sessionID: wirelessID, sequence: 5, firstSample: 4800, pcm: pcm)))
        recorder.finish(id: id)
        await fulfillment(of: [final], timeout: 3)
        let recordings = await client.recordings
        XCTAssertEqual(recordings.count, 1)
        XCTAssertEqual(recordings.first, Data(repeating: 0x10, count: pcm.count * 5))
    }

    func testCancelDropsPCMAndRejectsOldFramesInSuccessor() async throws {
        let client = Transcriber()
        let recorder = DictationRecorder(fluidVoice: client)
        let id = UUID(), wirelessID = UUID()
        let cancelled = expectation(description: "capture cancelled")
        try await recorder.start(id: id, transport: .wifi, wirelessSessionID: wirelessID, engine: .fluidVoice) { event in
            if case .failed = event { cancelled.fulfill() }
            if case .transcript = event { XCTFail("Cancelled recording must not transcribe") }
        }
        let pcm = Data(repeating: 0x10, count: WirelessMicrophoneProtocol.pcmBytesPerFrame)
        XCTAssertTrue(recorder.appendWirelessFrame(.init(sessionID: wirelessID, sequence: 0, firstSample: 0, pcm: pcm)))
        recorder.cancel(id: id)
        await fulfillment(of: [cancelled], timeout: 2)
        let next = UUID(), nextWireless = UUID()
        let done = expectation(description: "successor cancelled")
        try await recorder.start(id: next, transport: .wifi, wirelessSessionID: nextWireless, engine: .fluidVoice) { event in
            if case .failed = event { done.fulfill() }
        }
        XCTAssertFalse(recorder.appendWirelessFrame(.init(sessionID: wirelessID, sequence: 1, firstSample: 960, pcm: pcm)))
        recorder.cancel(id: id) // Cannot cancel successor.
        XCTAssertTrue(recorder.appendWirelessFrame(.init(sessionID: nextWireless, sequence: 0, firstSample: 0, pcm: pcm)))
        recorder.cancel(id: next)
        await fulfillment(of: [done], timeout: 2)
        let recordings = await client.recordings
        XCTAssertTrue(recordings.isEmpty)
    }
    private actor DelayedTranscriber: FluidVoiceTranscribing {
        var entered = false
        var pending: CheckedContinuation<String, Never>?
        func transcribe(pcm: Data, port: Int) async throws -> String {
            entered = true
            return await withCheckedContinuation { pending = $0 }
        }
        func complete() { pending?.resume(returning: "Obsolete upstream result"); pending = nil }
    }

    func testCancelledInferenceCannotDeliverItsLateResult() async throws {
        let client = DelayedTranscriber()
        let recorder = DictationRecorder(fluidVoice: client)
        let id = UUID(), wirelessID = UUID()
        let cancelled = expectation(description: "cancelled inference")
        let final = expectation(description: "no late upstream transcript")
        final.isInverted = true
        try await recorder.start(id: id, transport: .wifi, wirelessSessionID: wirelessID, engine: .fluidVoice) { event in
            if case .failed = event { cancelled.fulfill() }
            if case .transcript = event { final.fulfill() }
        }
        XCTAssertTrue(recorder.appendWirelessFrame(.init(sessionID: wirelessID, sequence: 0, firstSample: 0,
            pcm: Data(repeating: 0x10, count: WirelessMicrophoneProtocol.pcmBytesPerFrame))))
        let drained = await recorder.finishWireless(sessionID: wirelessID)
        XCTAssertTrue(drained)
        while !(await client.entered) { await Task.yield() }
        recorder.cancel(id: id)
        await fulfillment(of: [cancelled], timeout: 2)
        await client.complete()
        await fulfillment(of: [final], timeout: 0.1)
    }

}
