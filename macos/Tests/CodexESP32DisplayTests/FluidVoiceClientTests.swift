import Foundation
import XCTest
@testable import CodexESP32Display

final class FluidVoiceClientTests: XCTestCase {
    private actor HTTP: FluidVoiceHTTPTransport {
        var requests: [URLRequest] = []
        var body: Data
        var status: Int
        var error: Error?
        init(_ body: String = "{\"text\":\"Hello 世界\",\"confidence\":0.9,\"sampleCount\":16000,\"provider\":\"fixture\"}", status: Int = 200, error: Error? = nil) {
            self.body = Data(body.utf8); self.status = status; self.error = error
        }
        func send(_ request: URLRequest, limit: Int) async throws -> (Data, Int) {
            requests.append(request)
            if let error { throw error }
            guard body.count <= limit else { throw FluidVoiceError.responseTooLarge }
            return (body, status)
        }
    }

    func testWAVHeaderAndByteLimit() throws {
        var audio = FluidVoiceAudio()
        let pcm = Data([0x00, 0x10, 0x00, 0xF0])
        try audio.append(pcm)
        let wav = try FluidVoiceAudio.wav(audio.pcm)
        XCTAssertEqual(wav.count, 48)
        XCTAssertEqual(String(decoding: wav.prefix(4), as: UTF8.self), "RIFF")
        XCTAssertEqual(String(decoding: wav[8..<16], as: UTF8.self), "WAVEfmt ")
        XCTAssertEqual(wav[20..<36], Data([1,0,1,0,0x80,0xBB,0,0,0,0x77,1,0,2,0,16,0]))
        XCTAssertEqual(wav.suffix(4), pcm)
        XCTAssertThrowsError(try audio.append(Data([1])))
        XCTAssertThrowsError(try FluidVoiceAudio.wav(Data()))
        audio.reset()
        try audio.append(Data(repeating: 1, count: FluidVoiceAudio.maxPCMBytes))
        XCTAssertEqual(try FluidVoiceAudio.wav(audio.pcm).count, 5_760_044)
        XCTAssertThrowsError(try audio.append(Data([0, 0])))
        XCTAssertEqual(audio.pcm.count, FluidVoiceAudio.maxPCMBytes)
    }

    func testRawWAVRequestAndUnicodeResponse() async throws {
        let http = HTTP()
        let result = try await FluidVoiceClient(http: http).transcribe(pcm: Data([1, 0]), port: 47733)
        XCTAssertEqual(result, "Hello 世界")
        let requests = await http.requests
        let request = try XCTUnwrap(requests.first)
        XCTAssertEqual(request.url?.absoluteString, "http://127.0.0.1:47733/v1/transcribe")
        XCTAssertEqual(request.httpMethod, "POST")
        XCTAssertEqual(request.value(forHTTPHeaderField: "Content-Length"), "46")
        XCTAssertEqual(request.value(forHTTPHeaderField: "Content-Type"), "audio/wav")
        XCTAssertEqual(request.value(forHTTPHeaderField: "X-Filename"), "recording.wav")
        XCTAssertEqual(request.httpBody?.suffix(2), Data([1, 0]))
        XCTAssertEqual(request.timeoutInterval, 120)
    }

    func testMalformedEmptyOversizedAndRejectedResponses() async throws {
        let cases: [(String, Int, FluidVoiceError)] = [
            ("{}", 200, .invalidResponse),
            ("{\"text\":\" \",\"confidence\":0,\"sampleCount\":0,\"provider\":\"fixture\"}", 200, .noSpeech),
            ("{\"text\":\"x\",\"confidence\":0,\"sampleCount\":-1,\"provider\":\"fixture\"}", 200, .invalidResponse),
            ("{\"text\":\"" + String(repeating: "界", count: 6000) + "\",\"confidence\":0,\"sampleCount\":1,\"provider\":\"fixture\"}", 200, .responseTooLarge),
            (String(repeating: "x", count: 65537), 200, .responseTooLarge),
            ("private upstream error", 400, .rejected),
            ("", 307, .rejected)
        ]
        for (body, status, expected) in cases {
            do {
                _ = try await FluidVoiceClient(http: HTTP(body, status: status)).transcribe(pcm: Data([1, 0]), port: 47733)
                XCTFail("Expected \(expected)")
            } catch { XCTAssertEqual(error as? FluidVoiceError, expected) }
        }
    }

    func testHealthReturnsVersionWithoutTranscription() async throws {
        let http = HTTP("{\"status\":\"ok\",\"version\":\"1.6.9\"}")
        let version = try await FluidVoiceClient(http: http).health(port: 47733)
        XCTAssertEqual(version, "1.6.9")
        let requests = await http.requests
        XCTAssertEqual(requests.count, 1)
        XCTAssertEqual(requests.first?.url?.path, "/v1/health")
        XCTAssertNil(requests.first?.httpBody)
        XCTAssertEqual(requests.first?.timeoutInterval, 2)
    }

    func testInvalidPortDoesNotSendAndRefusedConnectionIsRecoverable() async throws {
        let http = HTTP(error: URLError(.cannotConnectToHost))
        let client = FluidVoiceClient(http: http)
        do { _ = try await client.transcribe(pcm: Data([1, 0]), port: 0); XCTFail() }
        catch { XCTAssertEqual(error as? FluidVoiceError, .invalidPort) }
        let requests = await http.requests
        XCTAssertTrue(requests.isEmpty)
        do { _ = try await client.transcribe(pcm: Data([1, 0]), port: 47733); XCTFail() }
        catch { XCTAssertEqual(error as? FluidVoiceError, .unavailable) }
        let uncertain = await client.uncertain
        XCTAssertFalse(uncertain)
    }

    func testTimeoutLatchesUntilExplicitReset() async throws {
        let http = HTTP(error: URLError(.timedOut))
        let client = FluidVoiceClient(http: http)
        for _ in 0..<2 {
            do { _ = try await client.transcribe(pcm: Data([1, 0]), port: 47733); XCTFail() }
            catch { XCTAssertEqual(error as? FluidVoiceError, .uncertain) }
        }
        var requests = await http.requests
        XCTAssertEqual(requests.count, 1, "No automatic repeat after uncertain completion")
        try await client.allowAnotherRequest()
        do { _ = try await client.transcribe(pcm: Data([1, 0]), port: 47733); XCTFail() }
        catch { XCTAssertEqual(error as? FluidVoiceError, .uncertain) }
        requests = await http.requests
        XCTAssertEqual(requests.count, 2)
    }

    private actor SuspendedHTTP: FluidVoiceHTTPTransport {
        var started = false
        var continuation: CheckedContinuation<(Data, Int), Error>?
        func send(_ request: URLRequest, limit: Int) async throws -> (Data, Int) {
            started = true
            return try await withCheckedThrowingContinuation { continuation = $0 }
        }
        func release() { continuation?.resume(throwing: URLError(.cancelled)); continuation = nil }
    }

    func testInFlightRequestCannotBeResetOrOverlapped() async throws {
        let http = SuspendedHTTP()
        let client = FluidVoiceClient(http: http)
        let first = Task { try await client.transcribe(pcm: Data([1, 0]), port: 47733) }
        while !(await http.started) { await Task.yield() }
        do { try await client.allowAnotherRequest(); XCTFail() }
        catch { XCTAssertEqual(error as? FluidVoiceError, .busy) }
        do { _ = try await client.transcribe(pcm: Data([1, 0]), port: 47733); XCTFail() }
        catch { XCTAssertEqual(error as? FluidVoiceError, .busy) }
        await http.release()
        do { _ = try await first.value; XCTFail() }
        catch { XCTAssertEqual(error as? FluidVoiceError, .uncertain) }
    }
}
