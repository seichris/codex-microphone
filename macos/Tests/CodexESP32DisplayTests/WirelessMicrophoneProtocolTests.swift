import Foundation
import XCTest
@testable import CodexESP32Display

final class WirelessMicrophoneProtocolTests: XCTestCase {
    private let credential = String(repeating: "a", count: 64)
    private let deviceID = "CESP32VOICE01"

    func testAudioFrameRoundTripsNetworkHeaderAndLittleEndianPCM() throws {
        let session = try XCTUnwrap(UUID(uuidString: "AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE"))
        var pcm = Data(count: WirelessMicrophoneProtocol.pcmBytesPerFrame)
        pcm[0] = 0x34; pcm[1] = 0x12
        let encoded = try WirelessMicrophoneProtocol.encodeAudioFrame(
            sessionID: session, sequence: 7, firstSample: 6_720, pcm: pcm
        )
        XCTAssertEqual(encoded.count, WirelessMicrophoneProtocol.maxAudioMessageLength)
        XCTAssertEqual(Array(encoded.prefix(4)), [0x43, 0x4D, 0x49, 0x43])
        XCTAssertEqual(Array(encoded[24..<28]), [0, 0, 0, 7])
        XCTAssertEqual(Array(encoded.suffix(2)), [0, 0])
        let decoded = try WirelessMicrophoneProtocol.decodeAudioFrame(encoded)
        XCTAssertEqual(decoded.sessionID, session)
        XCTAssertEqual(decoded.sequence, 7)
        XCTAssertEqual(decoded.firstSample, 6_720)
        XCTAssertEqual(decoded.pcm, pcm)
    }

    func testAudioFrameRejectsWrongSizeMagicVersionAndFlags() throws {
        let session = UUID()
        let pcm = Data(repeating: 0, count: WirelessMicrophoneProtocol.pcmBytesPerFrame)
        let encoded = try WirelessMicrophoneProtocol.encodeAudioFrame(sessionID: session, sequence: 0, firstSample: 0, pcm: pcm)
        for bad in [Data(encoded.dropLast()), Data([0x00] + encoded.dropFirst()), Data(encoded.enumerated().map { $0.offset == 4 ? 2 : $0.element }), Data(encoded.enumerated().map { $0.offset == 5 ? 1 : $0.element })] {
            XCTAssertThrowsError(try WirelessMicrophoneProtocol.decodeAudioFrame(bad))
        }
    }

    func testControlMessagesAreBoundedAndRejectUnknownFields() throws {
        let message = WirelessMicrophoneProtocol.ControlMessage(
            .start, requestID: "start-1", threadID: "01a06f9d-249c-7f43-b019-95324c366b8c", transport: "wifi"
        )
        let encoded = try WirelessMicrophoneProtocol.encodeControl(message)
        XCTAssertEqual(try WirelessMicrophoneProtocol.decodeControl(encoded), message)
        var object = try XCTUnwrap(JSONSerialization.jsonObject(with: encoded) as? [String: Any])
        object["audio"] = "must not be accepted"
        let withUnknown = try JSONSerialization.data(withJSONObject: object)
        XCTAssertThrowsError(try WirelessMicrophoneProtocol.decodeControl(withUnknown)) { error in
            XCTAssertEqual(error as? WirelessMicrophoneProtocol.ProtocolError, .unknownControlField("audio"))
        }
        XCTAssertThrowsError(try WirelessMicrophoneProtocol.decodeControl(Data(repeating: 0x78, count: 4_097)))
        var missingVersion = object
        missingVersion.removeValue(forKey: "version")
        XCTAssertThrowsError(try WirelessMicrophoneProtocol.decodeControl(try JSONSerialization.data(withJSONObject: missingVersion)))
    }

    func testSessionRequiresAuthenticationAndRejectsStaleOrSkippedFrames() throws {
        var session = WirelessMicrophoneSession(expectedCredential: credential, expectedDeviceID: deviceID)
        let hello = WirelessMicrophoneProtocol.ControlMessage(.hello, deviceID: deviceID, credential: credential)
        XCTAssertThrowsError(try session.authenticate(.init(.hello, deviceID: "other-board", credential: credential)))
        XCTAssertThrowsError(try session.start(.init(.start, requestID: "s", threadID: "01a06f9d-249c-7f43-b019-95324c366b8c", transport: "wifi")))
        XCTAssertEqual(try session.authenticate(hello).type, .capabilities)
        let prepared = try session.start(.init(.start, requestID: "s", threadID: "01a06f9d-249c-7f43-b019-95324c366b8c", transport: "wifi"))
        let commit = WirelessMicrophoneProtocol.ControlMessage(.commit, sessionID: prepared.sessionID, generation: prepared.generation)
        XCTAssertEqual(try session.commit(commit).type, .armed)
        let frameData = try WirelessMicrophoneProtocol.encodeAudioFrame(
            sessionID: try XCTUnwrap(UUID(uuidString: prepared.sessionID!)), sequence: 1,
            firstSample: UInt64(WirelessMicrophoneProtocol.samplesPerFrame),
            pcm: Data(repeating: 0, count: WirelessMicrophoneProtocol.pcmBytesPerFrame)
        )
        XCTAssertThrowsError(try session.acceptAudio(try WirelessMicrophoneProtocol.decodeAudioFrame(frameData)))
        let first = try WirelessMicrophoneProtocol.decodeAudioFrame(try WirelessMicrophoneProtocol.encodeAudioFrame(
            sessionID: try XCTUnwrap(UUID(uuidString: prepared.sessionID!)), sequence: 0, firstSample: 0,
            pcm: Data(repeating: 0, count: WirelessMicrophoneProtocol.pcmBytesPerFrame)
        ))
        XCTAssertEqual(try session.acceptAudio(first)?.type, .listening)
        XCTAssertThrowsError(try session.acceptAudio(first))
    }

    func testSessionStopIsExclusiveAndIdempotent() throws {
        var session = WirelessMicrophoneSession(expectedCredential: credential)
        _ = try session.authenticate(.init(.hello, deviceID: deviceID, credential: credential))
        let prepared = try session.start(.init(.start, requestID: "s", threadID: "01a06f9d-249c-7f43-b019-95324c366b8c", transport: "wifi"))
        _ = try session.commit(.init(.commit, sessionID: prepared.sessionID, generation: prepared.generation))
        let stop = WirelessMicrophoneProtocol.ControlMessage(.stop, sessionID: prepared.sessionID, generation: prepared.generation, finalSequence: 0)
        XCTAssertEqual(try session.stop(stop).type, .stopped)
        XCTAssertEqual(try session.stop(stop).type, .stopped)
        XCTAssertThrowsError(try session.commit(.init(.commit, sessionID: prepared.sessionID, generation: prepared.generation)))
        let next = try session.start(.init(
            .start, requestID: "next", threadID: "01a06f9d-249c-7f43-b019-95324c366b8c", transport: "wifi"
        ))
        XCTAssertNotEqual(next.sessionID, prepared.sessionID)
    }
}
