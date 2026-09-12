import Foundation
import Network
import XCTest
@testable import CodexESP32Display

/// Exercises URLSession against real loopback HTTP, never the installed app.
final class FluidVoiceHTTPTests: XCTestCase {
    private final class Server: @unchecked Sendable {
        let listener: NWListener
        let queue = DispatchQueue(label: "fluidvoice-http-fixture")
        let reply: @Sendable (Data) -> Data
        init(reply: @escaping @Sendable (Data) -> Data) throws {
            let parameters = NWParameters.tcp
            parameters.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: .any)
            listener = try NWListener(using: parameters)
            self.reply = reply
        }
        func start() async throws -> Int {
            try await withCheckedThrowingContinuation { continuation in
                listener.stateUpdateHandler = { [weak self] state in
                    guard let self else { return }
                    if case .ready = state {
                        self.listener.stateUpdateHandler = nil
                        continuation.resume(returning: Int(self.listener.port!.rawValue))
                    } else if case let .failed(error) = state {
                        self.listener.stateUpdateHandler = nil
                        continuation.resume(throwing: error)
                    }
                }
                listener.newConnectionHandler = { [weak self] connection in
                    guard let self else { connection.cancel(); return }
                    connection.start(queue: self.queue)
                    self.receive(connection, buffer: Data())
                }
                listener.start(queue: queue)
            }
        }
        func receive(_ connection: NWConnection, buffer: Data) {
            connection.receive(minimumIncompleteLength: 1, maximumLength: 8192) { [weak self] data, _, done, error in
                guard let self, error == nil else { connection.cancel(); return }
                var buffer = buffer
                if let data { buffer.append(data) }
                guard buffer.count <= 64 * 1024 else { connection.cancel(); return }
                if let boundary = buffer.range(of: Data("\r\n\r\n".utf8)) {
                    let headers = String(decoding: buffer[..<boundary.lowerBound], as: UTF8.self)
                    let length = headers.components(separatedBy: "\r\n").first { $0.lowercased().hasPrefix("content-length:") }
                        .flatMap { Int($0.split(separator: ":")[1].trimmingCharacters(in: .whitespaces)) } ?? 0
                    if buffer.count >= boundary.upperBound + length {
                        connection.send(content: self.reply(buffer), completion: .contentProcessed { _ in connection.cancel() })
                        return
                    }
                }
                if done { connection.cancel() } else { self.receive(connection, buffer: buffer) }
            }
        }
        func stop() { listener.cancel() }
    }

    func testRealWAVUploadAndBoundedJSONResponse() async throws {
        let seen = expectation(description: "HTTP WAV upload")
        let body = "{\"text\":\"real HTTP fixture\",\"confidence\":1,\"sampleCount\":1,\"provider\":\"fixture\"}"
        let server = try Server { request in
            let headerEnd = request.range(of: Data("\r\n\r\n".utf8))!.upperBound
            let headers = String(decoding: request[..<headerEnd], as: UTF8.self)
            XCTAssertTrue(headers.hasPrefix("POST /v1/transcribe HTTP/1.1"))
            XCTAssertTrue(headers.lowercased().contains("content-type: audio/wav"))
            XCTAssertEqual(request[headerEnd...].prefix(4), Data("RIFF".utf8))
            XCTAssertEqual(request.suffix(2), Data([0x00, 0x10]))
            seen.fulfill()
            return Data("HTTP/1.1 200 OK\r\nContent-Length: \(body.utf8.count)\r\nConnection: close\r\n\r\n\(body)".utf8)
        }
        let port = try await server.start()
        defer { server.stop() }
        let result = try await FluidVoiceClient().transcribe(pcm: Data([0x00, 0x10]), port: port)
        XCTAssertEqual(result, "real HTTP fixture")
        await fulfillment(of: [seen], timeout: 2)
    }

    func testRedirectDoesNotForwardAudio() async throws {
        let sinkSeen = expectation(description: "redirect target must not receive audio")
        sinkSeen.isInverted = true
        let sink = try Server { _ in sinkSeen.fulfill(); return Data("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n".utf8) }
        let sinkPort = try await sink.start()
        defer { sink.stop() }
        let server = try Server { _ in
            Data("HTTP/1.1 307 Temporary Redirect\r\nLocation: http://127.0.0.1:\(sinkPort)/leak\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".utf8)
        }
        let port = try await server.start()
        defer { server.stop() }
        do { _ = try await FluidVoiceClient().transcribe(pcm: Data([0, 16]), port: port); XCTFail() }
        catch { XCTAssertEqual(error as? FluidVoiceError, .rejected) }
        await fulfillment(of: [sinkSeen], timeout: 0.1)
    }

    func testChunkedResponseLimitIsEnforcedWithoutContentLength() async throws {
        let server = try Server { _ in
            Data(("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n" +
                  "10001\r\n" + String(repeating: "a", count: 65537) + "\r\n0\r\n\r\n").utf8)
        }
        let port = try await server.start()
        defer { server.stop() }
        do { _ = try await FluidVoiceClient().transcribe(pcm: Data([0, 16]), port: port); XCTFail() }
        catch { XCTAssertEqual(error as? FluidVoiceError, .responseTooLarge) }
    }
}
