import Foundation

/// Engine selection is independent of USB/Wi-Fi transport and frozen per take.
enum DictationEngine: String, CaseIterable, Identifiable, Sendable {
    case appleSpeech = "apple-speech"
    case fluidVoice = "fluidvoice"
    var id: String { rawValue }
    var title: String { self == .appleSpeech ? "Apple Speech" : "FluidVoice (local API)" }
}

enum FluidVoiceError: LocalizedError, Equatable {
    case invalidAudio, tooMuchAudio, responseTooLarge, invalidResponse, noSpeech
    case unavailable, rejected, busy, uncertain, invalidPort
    var errorDescription: String? {
        switch self {
        case .invalidAudio: return "FluidVoice requires 48 kHz mono PCM16 audio."
        case .tooMuchAudio: return "Recording exceeded the 60-second audio limit."
        case .responseTooLarge: return "FluidVoice returned too much text. Record a shorter message."
        case .invalidResponse: return "FluidVoice returned an unsupported response. Check its version and local API."
        case .noSpeech: return "No speech was recognized. Check the microphone level and try again."
        case .unavailable: return "FluidVoice is unavailable. Open it, enable its local API, and check the port."
        case .rejected: return "FluidVoice could not transcribe this recording. Check its selected speech model."
        case .busy: return "Wait for the current FluidVoice request to finish."
        case .uncertain: return "The FluidVoice request was interrupted. It may still be running. Check FluidVoice, then allow another request in Voice Settings."
        case .invalidPort: return "Enter a local API port from 1 to 65535."
        }
    }
}

/// The companion retains at most 60 seconds in RAM; FluidVoice may use temp files.
struct FluidVoiceAudio {
    static let sampleRate = 48_000
    static let maxPCMBytes = sampleRate * 2 * 60
    private(set) var pcm = Data()

    mutating func append(_ data: Data) throws {
        guard !data.isEmpty, data.count.isMultiple(of: 2) else { throw FluidVoiceError.invalidAudio }
        guard data.count <= Self.maxPCMBytes - pcm.count else { throw FluidVoiceError.tooMuchAudio }
        pcm.append(data)
    }

    mutating func reset() { pcm = Data() }

    static func wav(_ pcm: Data) throws -> Data {
        guard !pcm.isEmpty, pcm.count.isMultiple(of: 2) else { throw FluidVoiceError.invalidAudio }
        guard pcm.count <= maxPCMBytes else { throw FluidVoiceError.tooMuchAudio }
        var data = Data()
        func text(_ value: String) { data.append(contentsOf: value.utf8) }
        func u16(_ value: UInt16) { withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) } }
        func u32(_ value: UInt32) { withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) } }
        text("RIFF"); u32(UInt32(pcm.count + 36)); text("WAVEfmt ")
        u32(16); u16(1); u16(1); u32(UInt32(sampleRate)); u32(UInt32(sampleRate * 2))
        u16(2); u16(16); text("data"); u32(UInt32(pcm.count)); data.append(pcm)
        return data
    }
}

protocol FluidVoiceHTTPTransport: Sendable {
    func send(_ request: URLRequest, limit: Int) async throws -> (Data, Int)
}

private final class NoRedirects: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
    func urlSession(_ session: URLSession, task: URLSessionTask,
                    willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest,
                    completionHandler: @escaping (URLRequest?) -> Void) {
        completionHandler(nil)
    }
}

struct FluidVoiceHTTP: FluidVoiceHTTPTransport {
    func send(_ request: URLRequest, limit: Int) async throws -> (Data, Int) {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.urlCache = nil
        configuration.httpCookieStorage = nil
        configuration.connectionProxyDictionary = [:]
        configuration.timeoutIntervalForResource = request.timeoutInterval
        let session = URLSession(configuration: configuration, delegate: NoRedirects(), delegateQueue: nil)
        defer { session.invalidateAndCancel() }
        let (bytes, response) = try await session.bytes(for: request)
        guard let response = response as? HTTPURLResponse else { throw FluidVoiceError.invalidResponse }
        // Never retain/log error bodies or follow a redirect with microphone audio.
        guard response.statusCode == 200 else { return (Data(), response.statusCode) }
        guard response.expectedContentLength <= Int64(limit) else { throw FluidVoiceError.responseTooLarge }
        var body = Data()
        for try await byte in bytes {
            try Task.checkCancellation()
            guard body.count < limit else { throw FluidVoiceError.responseTooLarge }
            body.append(byte)
        }
        return (body, response.statusCode)
    }
}

protocol FluidVoiceTranscribing: Sendable {
    func transcribe(pcm: Data, port: Int) async throws -> String
}

/// One in-flight call even across recorder cancellation. An uncertain upstream
/// request never gets an automatic retry; health alone cannot clear that latch.
actor FluidVoiceClient: FluidVoiceTranscribing {
    static let shared = FluidVoiceClient()
    private let http: any FluidVoiceHTTPTransport
    private var active = false
    private(set) var uncertain = false

    init(http: any FluidVoiceHTTPTransport = FluidVoiceHTTP()) { self.http = http }

    static func request(path: String, port: Int, timeout: TimeInterval) throws -> URLRequest {
        guard (1...65535).contains(port) else { throw FluidVoiceError.invalidPort }
        let url = URL(string: "http://127.0.0.1:\(port)\(path)")!
        var request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: timeout)
        request.setValue("application/json", forHTTPHeaderField: "Accept")
        return request
    }

    func health(port: Int) async throws -> String {
        guard !active else { throw FluidVoiceError.busy }
        struct Health: Decodable { let status: String; let version: String }
        do {
            let (data, status) = try await http.send(Self.request(path: "/v1/health", port: port, timeout: 2), limit: 4096)
            guard status == 200, let health = try? JSONDecoder().decode(Health.self, from: data),
                  health.status == "ok", !health.version.isEmpty, health.version.utf8.count <= 64 else {
                throw FluidVoiceError.invalidResponse
            }
            return health.version
        } catch let error as FluidVoiceError { throw error }
        catch { throw FluidVoiceError.unavailable }
    }

    func allowAnotherRequest() throws {
        guard !active else { throw FluidVoiceError.busy }
        uncertain = false
    }

    func transcribe(pcm: Data, port: Int) async throws -> String {
        guard !active else { throw FluidVoiceError.busy }
        guard !uncertain else { throw FluidVoiceError.uncertain }
        var request = try Self.request(path: "/v1/transcribe", port: port, timeout: 120)
        request.httpMethod = "POST"
        request.httpBody = try FluidVoiceAudio.wav(pcm)
        request.setValue("audio/wav", forHTTPHeaderField: "Content-Type")
        request.setValue("recording.wav", forHTTPHeaderField: "X-Filename")
        request.setValue(String(request.httpBody!.count), forHTTPHeaderField: "Content-Length")
        try Task.checkCancellation()
        active = true
        defer { active = false }
        let data: Data
        let status: Int
        do { (data, status) = try await http.send(request, limit: 64 * 1024) }
        catch let error as FluidVoiceError { throw error }
        catch {
            if let urlError = error as? URLError,
               [.cannotConnectToHost, .cannotFindHost, .notConnectedToInternet].contains(urlError.code) {
                throw FluidVoiceError.unavailable
            }
            uncertain = true
            throw FluidVoiceError.uncertain
        }
        guard status == 200 else { throw FluidVoiceError.rejected }
        struct Transcript: Decodable {
            let text: String
            let confidence: Float
            let sampleCount: Int
            let provider: String
        }
        guard let result = try? JSONDecoder().decode(Transcript.self, from: data),
              result.confidence.isFinite, result.sampleCount >= 0,
              !result.provider.isEmpty else { throw FluidVoiceError.invalidResponse }
        guard result.text.utf8.count <= 16 * 1024 else { throw FluidVoiceError.responseTooLarge }
        let text = result.text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { throw FluidVoiceError.noSpeech }
        return text
    }
}
