import AVFoundation
import Darwin
import Foundation
import Speech

/// Capture/recognition state is confined to queue. FluidVoice audio is bounded in RAM.
final class DictationRecorder: NSObject, AVCaptureAudioDataOutputSampleBufferDelegate, @unchecked Sendable {
    static let deviceID = "AppleUSBAudioEngine:Codex ESP32 Display:Waveshare Voice Microphone:CESP32VOICE01:1"
    enum Event: Sendable {
        case prepared
        case recording, finishing
        case transcript(String, final: Bool)
        case level(Float)
        case failed(String)
    }
    private let queue = DispatchQueue(label: "display.dictation.capture")
    private let wirelessIngress: WirelessAudioIngress
    private var audioEnded = false
    private static var monotonicNow: TimeInterval { ProcessInfo.processInfo.systemUptime }

    private let fluidVoice: any FluidVoiceTranscribing
    private var engine: DictationEngine = .appleSpeech
    private var fluidVoicePort = 47733
    private var fluidAudio = FluidVoiceAudio()
    private var fluidTask: Task<Void, Never>?

    init(fluidVoice: any FluidVoiceTranscribing = FluidVoiceClient.shared) {
        self.fluidVoice = fluidVoice
        wirelessIngress = WirelessAudioIngress(queue: queue)
        super.init()
    }
    private var session: AVCaptureSession?
    private var request: SFSpeechAudioBufferRecognitionRequest?
    private var task: SFSpeechRecognitionTask?
    private var recognizer: SFSpeechRecognizer?
    private var generation: UUID?
    private var event: (@Sendable (Event) -> Void)?
    private var startCompletion: CheckedContinuation<Void, Error>?
    private var finishing = false
    private var samples = 0
    private var peak: Float = 0
    private var lastLevel: TimeInterval = -.infinity
    private var startDeadline: TimeInterval = 0
    private var lastBufferAt: TimeInterval = 0
    private var transcriptAccumulator = DictationTranscriptAccumulator()
    private var transport: DictationTransport = .usb
    private var wirelessSessionID: UUID?
    private var wirelessFormat: AVAudioFormat?
    private var wirelessDidEmitRecording = false

    static var device: AVCaptureDevice? { AVCaptureDevice(uniqueID: deviceID) }
    static var permissionReady: Bool {
        microphonePermissionReady && speechPermissionReady
    }
    static var microphonePermissionReady: Bool {
        AVCaptureDevice.authorizationStatus(for: .audio) == .authorized
    }
    static var speechPermissionReady: Bool {
        SFSpeechRecognizer.authorizationStatus() == .authorized
    }
    static var onDeviceAvailable: Bool {
        SFSpeechRecognizer(locale: Locale(identifier: "en-US"))?.supportsOnDeviceRecognition == true
    }

    func start(id: UUID, event: @escaping @Sendable (Event) -> Void) async throws {
        try await start(id: id, transport: .usb, wirelessSessionID: nil, event: event)
    }

    func start(
        id: UUID,
        transport: DictationTransport,
        wirelessSessionID: UUID? = nil,
        engine: DictationEngine = .appleSpeech,
        fluidVoicePort: Int = 47733,
        event: @escaping @Sendable (Event) -> Void
    ) async throws {
        try await withCheckedThrowingContinuation { (completion: CheckedContinuation<Void, Error>) in
            queue.async {
                guard self.generation == nil else {
                    completion.resume(throwing: DictationError.message("Dictation is already active.")); return
                }
                self.generation = id
                self.event = event
                self.startCompletion = completion
                self.startDeadline = Self.monotonicNow + 2
                self.lastBufferAt = Self.monotonicNow
                self.finishing = false
                self.audioEnded = false
                self.transport = transport
                self.engine = engine
                self.fluidVoicePort = fluidVoicePort
                self.fluidAudio.reset()
                self.wirelessSessionID = wirelessSessionID
                self.wirelessFormat = nil
                self.wirelessDidEmitRecording = false
                self.samples = 0
                self.peak = 0
                self.transcriptAccumulator.reset()
                do {
                    if engine == .appleSpeech {
                        guard Self.speechPermissionReady else { throw DictationError.message("Allow Speech Recognition in Voice Settings.") }
                        guard let recognizer = SFSpeechRecognizer(locale: Locale(identifier: "en-US")), recognizer.supportsOnDeviceRecognition else {
                            throw DictationError.message("On-device English speech recognition is unavailable on this Mac.")
                        }
                        let request = SFSpeechAudioBufferRecognitionRequest()
                        request.requiresOnDeviceRecognition = true
                        request.shouldReportPartialResults = true
                        request.taskHint = .dictation
                        self.request = request
                        self.recognizer = recognizer
                        self.task = recognizer.recognitionTask(with: request) { result, error in
                            self.queue.async {
                                guard self.generation == id else { return }
                                if let result {
                                    let segments = result.bestTranscription.segments
                                    let start = segments.first?.timestamp
                                    let end = segments.last.map { $0.timestamp + $0.duration }
                                    let audioRange: Range<TimeInterval>?
                                    if let start, let end, start.isFinite, end.isFinite, start >= 0, end > start {
                                        audioRange = start..<end
                                    } else {
                                        audioRange = nil
                                    }
                                    let transcript = self.transcriptAccumulator.update(
                                        result.bestTranscription.formattedString,
                                        audioRange: audioRange
                                    )
                                    self.event?(.transcript(transcript, final: result.isFinal))
                                    if result.isFinal { self.cleanup(); return }
                                }
                                if let error { self.fail("Speech recognition failed: \(error.localizedDescription)") }
                            }
                        }
                    }
                    if transport == .usb {
                        guard Self.microphonePermissionReady else { throw DictationError.message("Allow Microphone access in Voice Settings.") }
                        guard let device = Self.device else { throw DictationError.message("Connect the Waveshare Voice Microphone by USB.") }
                        let capture = AVCaptureSession()
                        capture.beginConfiguration()
                        let input = try AVCaptureDeviceInput(device: device)
                        let output = AVCaptureAudioDataOutput()
                        output.audioSettings = [AVFormatIDKey: kAudioFormatLinearPCM, AVLinearPCMBitDepthKey: 16,
                            AVLinearPCMIsFloatKey: false, AVLinearPCMIsBigEndianKey: false, AVNumberOfChannelsKey: 1,
                            AVSampleRateKey: 48000]
                        guard capture.canAddInput(input), capture.canAddOutput(output) else {
                            capture.commitConfiguration()
                            throw DictationError.message("Could not configure the Waveshare microphone.")
                        }
                        capture.addInput(input)
                        capture.addOutput(output)
                        output.setSampleBufferDelegate(self, queue: self.queue)
                        capture.commitConfiguration()
                        self.session = capture
                        capture.startRunning()
                        guard Self.monotonicNow < self.startDeadline else { throw DictationError.message("The microphone took too long to start. Try again.") }
                        self.checkBufferLiveness(id)
                        guard capture.isRunning else { throw DictationError.message("The Waveshare microphone did not start.") }
                        // A successful USB acknowledgement requires a real sample buffer.
                        self.queue.asyncAfter(deadline: .now() + 1.5) {
                            if self.generation == id, self.startCompletion != nil { self.fail("No audio buffers arrived from the Waveshare microphone.") }
                        }
                    } else {
                        guard let wirelessSessionID else { throw DictationError.message("Wireless microphone session is missing.") }
                        self.wirelessSessionID = wirelessSessionID
                        self.wirelessFormat = AVAudioFormat(
                            commonFormat: .pcmFormatInt16,
                            sampleRate: Double(WirelessMicrophoneProtocol.sampleRate),
                            channels: AVAudioChannelCount(WirelessMicrophoneProtocol.channels),
                            interleaved: false
                        )
                        guard self.wirelessFormat != nil else { throw DictationError.message("Could not configure the Wi-Fi microphone format.") }
                        // The receiver is prepared before the board commits/arms
                        // the session. Do not claim listening until the first
                        // validated frame arrives through appendWirelessFrame.
                        self.startDeadline = Self.monotonicNow + 5
                        self.wirelessIngress.open(sessionID: wirelessSessionID)
                        self.event?(.prepared)
                        if let completion = self.startCompletion {
                            self.startCompletion = nil
                            completion.resume()
                        }
                        self.checkBufferLiveness(id)
                    }
                    self.queue.asyncAfter(deadline: .now() + 55) {
                        if self.generation == id { self.finishOnQueue() }
                    }
                } catch { self.fail(error.localizedDescription) }
            }
        }
    }

    func finish(id: UUID) {
        queue.async { if self.generation == id { self.finishOnQueue() } }
    }

    func cancel(id: UUID) {
        queue.async {
            guard self.generation == id else { return }
            self.fail("Recording cancelled.")
        }
    }

    func cancel(message: String, sessionID: UUID) {
        wirelessIngress.close(sessionID: sessionID)
        queue.async {
            guard self.transport == .wifi, self.wirelessSessionID == sessionID else { return }
            self.fail(message)
        }
    }

    /// Completion means all admitted frames drained and endAudio was called,
    /// not that Speech finalized or that a composer accepted the transcript.
    func finishWireless(sessionID: UUID) async -> Bool {
        await withCheckedContinuation { completion in
            let accepted = wirelessIngress.close(sessionID: sessionID) {
                guard self.generation != nil, self.transport == .wifi,
                      self.wirelessSessionID == sessionID else {
                    completion.resume(returning: false); return
                }
                self.finishOnQueue()
                DictationDiagnostics.record("wifi-recorder-drained", samples: self.samples)
                completion.resume(returning: true)
            }
            if !accepted { completion.resume(returning: false) }
        }
    }

    @discardableResult
    func appendWirelessFrame(_ frame: WirelessMicrophoneProtocol.AudioFrame) -> Bool {
        guard frame.pcm.count == WirelessMicrophoneProtocol.pcmBytesPerFrame else { return false }
        return wirelessIngress.enqueue(sessionID: frame.sessionID) {
            guard self.transport == .wifi, self.generation != nil, !self.finishing,
                  frame.sessionID == self.wirelessSessionID else { return }
            self.consumeWirelessFrame(frame)
        }
    }

    private func endAudioOnce() {
        guard !audioEnded, let request else { return }
        audioEnded = true
        request.endAudio()
    }

    private func finishOnQueue() {
        guard generation != nil, !finishing else { return }
        finishing = true
        if let wirelessSessionID { wirelessIngress.close(sessionID: wirelessSessionID) }
        DictationDiagnostics.record("finish", samples: samples, peak: peak)
        event?(.finishing)
        session?.stopRunning()
        if engine == .fluidVoice {
            guard let id = generation else { return }
            let pcm = fluidAudio.pcm
            fluidAudio.reset()
            guard !pcm.isEmpty, peak >= 0.0001 else {
                fail("The microphone delivered silence. Check the input level and try again.")
                return
            }
            let port = fluidVoicePort
            fluidTask = Task { [weak self, fluidVoice] in
                let result: Result<String, Error>
                do { result = .success(try await fluidVoice.transcribe(pcm: pcm, port: port)) }
                catch { result = .failure(error) }
                self?.queue.async { [weak self] in
                    guard let self, self.generation == id else { return }
                    switch result {
                    case let .success(text):
                        self.event?(.transcript(text, final: true))
                        self.cleanup()
                    case let .failure(error): self.fail(error.localizedDescription)
                    }
                }
            }
            return
        }
        endAudioOnce()
        let id = generation
        queue.asyncAfter(deadline: .now() + 10) {
            guard self.generation == id else { return }
            self.fail(self.samples == 0 || self.peak < 0.0001
                ? "The microphone delivered silence. Try again after the device shows LISTENING."
                : "Transcription did not finish. Any partial text is preserved for review.")
        }
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard generation != nil, !finishing, transport == .usb,
              session?.outputs.contains(where: { $0 === output }) == true else { return }
        if startCompletion != nil, Self.monotonicNow >= startDeadline {
            fail("The microphone took too long to deliver audio. Try again.")
            return
        }
        lastBufferAt = Self.monotonicNow
        samples += CMSampleBufferGetNumSamples(sampleBuffer)
        if let block = CMSampleBufferGetDataBuffer(sampleBuffer), CMBlockBufferGetDataLength(block) > 0 {
            let length = CMBlockBufferGetDataLength(block)
            var data = Data(count: length)
            let status = data.withUnsafeMutableBytes { CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: $0.baseAddress!) }
            if status == kCMBlockBufferNoErr {
                if engine == .fluidVoice {
                    guard let description = CMSampleBufferGetFormatDescription(sampleBuffer),
                          let format = CMAudioFormatDescriptionGetStreamBasicDescription(description)?.pointee,
                          format.mSampleRate == 48000, format.mChannelsPerFrame == 1,
                          format.mBitsPerChannel == 16, format.mFormatID == kAudioFormatLinearPCM,
                          format.mFormatFlags & (kAudioFormatFlagIsFloat | kAudioFormatFlagIsBigEndian) == 0,
                          format.mFormatFlags & kAudioFormatFlagIsSignedInteger != 0 else {
                        fail(FluidVoiceError.invalidAudio.localizedDescription); return
                    }
                    do { try fluidAudio.append(data) }
                    catch { fail(error.localizedDescription); return }
                }
                let level = data.withUnsafeBytes { bytes -> Float in
                    var result: Float = 0
                    for offset in stride(from: 0, to: max(0, bytes.count - 1), by: 2) {
                        let value = Int16(littleEndian: bytes.loadUnaligned(fromByteOffset: offset, as: Int16.self))
                        result = max(result, abs(Float(value)) / 32768)
                    }
                    return result
                }
                peak = max(peak, level)
                if Self.monotonicNow - lastLevel > 0.15 { lastLevel = Self.monotonicNow; event?(.level(level)) }
            } else if engine == .fluidVoice {
                fail(FluidVoiceError.invalidAudio.localizedDescription); return
            }
        } else if engine == .fluidVoice {
            fail(FluidVoiceError.invalidAudio.localizedDescription); return
        }
        request?.appendAudioSampleBuffer(sampleBuffer)
        if let completion = startCompletion {
            startCompletion = nil
            DictationDiagnostics.record("recording", samples: samples, peak: peak)
            event?(.recording)
            completion.resume()
        }
    }

    private func consumeWirelessFrame(_ frame: WirelessMicrophoneProtocol.AudioFrame) {
        guard let format = wirelessFormat,
              let buffer = AVAudioPCMBuffer(
                pcmFormat: format,
                frameCapacity: AVAudioFrameCount(WirelessMicrophoneProtocol.samplesPerFrame)
              ),
              let channel = buffer.int16ChannelData?.pointee else {
            fail("The Wi-Fi microphone delivered an unsupported audio format.")
            return
        }
        frame.pcm.withUnsafeBytes { bytes in
            guard let baseAddress = bytes.baseAddress else { return }
            memcpy(channel, baseAddress, frame.pcm.count)
        }
        buffer.frameLength = AVAudioFrameCount(WirelessMicrophoneProtocol.samplesPerFrame)
        lastBufferAt = Self.monotonicNow
        samples += WirelessMicrophoneProtocol.samplesPerFrame
        let level = frame.pcm.withUnsafeBytes { bytes -> Float in
            var result: Float = 0
            for offset in stride(from: 0, to: max(0, bytes.count - 1), by: 2) {
                let value = Int16(littleEndian: bytes.loadUnaligned(fromByteOffset: offset, as: Int16.self))
                result = max(result, abs(Float(value)) / 32768)
            }
            return result
        }
        peak = max(peak, level)
        if Self.monotonicNow - lastLevel > 0.15 { lastLevel = Self.monotonicNow; event?(.level(level)) }
        if engine == .fluidVoice {
            do { try fluidAudio.append(frame.pcm) }
            catch { fail(error.localizedDescription); return }
        } else { request?.append(buffer) }
        if !wirelessDidEmitRecording { DictationDiagnostics.record((engine == .fluidVoice ? "wifi-first-fluidvoice-buffer" : "wifi-first-speech-append"), samples: samples) }
        if startCompletion != nil {
            startCompletion = nil
            DictationDiagnostics.record("recording", samples: samples, peak: peak)
            event?(.recording)
        } else if generation != nil && !wirelessDidEmitRecording {
            wirelessDidEmitRecording = true
            event?(.recording)
        }
    }

    private func checkBufferLiveness(_ id: UUID) {
        queue.asyncAfter(deadline: .now() + 1) {
            guard self.generation == id, !self.finishing else { return }
            if Self.monotonicNow - self.lastBufferAt > 2 {
                if self.transport == .wifi && self.samples == 0 && Self.monotonicNow < self.startDeadline {
                    self.checkBufferLiveness(id)
                } else {
                    self.fail(self.transport == .wifi
                        ? "Audio stopped arriving from the Wi-Fi microphone. Check the Mac pairing and Wi-Fi connection."
                        : "Audio stopped arriving from the Waveshare microphone. Check the USB connection.")
                }
            } else { self.checkBufferLiveness(id) }
        }
    }

    private func fail(_ message: String) { DictationDiagnostics.record("capture-or-recognition-failed", samples: samples, peak: peak); event?(.failed(message)); cleanup() }

    private func cleanup() {
        if let wirelessSessionID { wirelessIngress.close(sessionID: wirelessSessionID) }
        generation = nil
        fluidTask?.cancel()
        fluidTask = nil
        fluidAudio.reset()
        transcriptAccumulator.reset()
        session?.stopRunning()
        session = nil
        endAudioOnce()
        request = nil
        task?.cancel()
        task = nil
        recognizer = nil
        transport = .usb
        wirelessSessionID = nil
        wirelessFormat = nil
        wirelessDidEmitRecording = false
        event = nil
        if let completion = startCompletion {
            startCompletion = nil
            completion.resume(throwing: DictationError.message("Recording could not start. Check Voice Settings for the cause."))
        }
    }
}
