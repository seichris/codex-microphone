import AppKit
import AVFoundation
import Combine
import Speech
import SwiftUI

@MainActor
final class DictationModel: ObservableObject {
    @Published private(set) var session = DictationSession()
    @Published var draftText = ""
    @Published private(set) var level: Float = 0
    @Published private(set) var handoffMessage: String?
    @Published private(set) var isOpeningDraft = false
    @Published private(set) var permissionRevision = 0
    // One memory-only recovery copy. A successful URL open is not a composer
    // acknowledgement. Never automatically replay an uncertain handoff.
    struct HandoffRecovery { let threadID: String; let text: String }
    @Published private(set) var lastAttemptedHandoff: HandoffRecovery?
    var onStateChange: (() -> Void)?
    private let recorder = DictationRecorder()
    private let defaults: UserDefaults
    @Published private(set) var sessionEngine: DictationEngine
    @Published private(set) var fluidVoiceAvailable = false
    @Published private(set) var fluidVoiceStatus = "FluidVoice has not been checked."
    @Published private(set) var fluidVoiceUncertain = false
    private var readinessRevision = 0
    private let wirelessServer: WirelessMicrophoneServer?
    private var window: NSWindow?
    private let openDraftLink: @MainActor (URL) async throws -> Void

    init(session: DictationSession = DictationSession(),
         wirelessServer: WirelessMicrophoneServer? = nil,
         defaults: UserDefaults = .standard,
         sessionEngine: DictationEngine = .appleSpeech,
         openDraftLink: @escaping @MainActor (URL) async throws -> Void = CodexAppLink.open) {
        self.defaults = defaults
        self.sessionEngine = sessionEngine
        self.session = session
        self.draftText = session.text
        self.wirelessServer = wirelessServer
        self.openDraftLink = openDraftLink
    }

    var engine: DictationEngine {
        DictationEngine(rawValue: defaults.string(forKey: "VoiceEngine") ?? "") ?? .appleSpeech
    }
    var fluidVoicePort: Int { Int(defaults.string(forKey: "FluidVoicePort") ?? "47733") ?? 0 }
    private var recognitionReady: Bool {
        engine == .fluidVoice ? fluidVoiceAvailable
            : DictationRecorder.speechPermissionReady && DictationRecorder.onDeviceAvailable
    }

    func refreshFluidVoice() async {
        readinessRevision += 1
        fluidVoiceAvailable = false
        let revision = readinessRevision
        let port = fluidVoicePort
        do {
            let version = try await FluidVoiceClient.shared.health(port: port)
            let uncertain = await FluidVoiceClient.shared.uncertain
            guard revision == readinessRevision, port == fluidVoicePort else { return }
            fluidVoiceUncertain = uncertain
            fluidVoiceAvailable = !uncertain
            fluidVoiceStatus = uncertain ? FluidVoiceError.uncertain.localizedDescription
                : "FluidVoice \(version) API reachable. A recording will check the selected model."
        } catch {
            guard revision == readinessRevision, port == fluidVoicePort else { return }
            fluidVoiceAvailable = false
            fluidVoiceUncertain = await FluidVoiceClient.shared.uncertain
            fluidVoiceStatus = error.localizedDescription
        }
        onStateChange?()
    }

    func allowAnotherFluidVoiceRequest() async {
        do { try await FluidVoiceClient.shared.allowAnotherRequest() }
        catch { fluidVoiceStatus = error.localizedDescription; return }
        await refreshFluidVoice()
    }

    var usbReady: Bool {
        DictationRecorder.microphonePermissionReady && DictationRecorder.device != nil && recognitionReady
    }
    var wirelessReady: Bool {
        recognitionReady && wirelessServer?.isReady == true
    }
    var wirelessPairingConfigured: Bool { wirelessServer?.configurationPairing != nil }
    var wirelessReadinessTitle: String {
        if wirelessReady { return "Wi-Fi microphone listener ready" }
        if wirelessServer?.isReady == true { return "Wi-Fi listener ready; transcription needs setup" }
        return wirelessPairingConfigured ? "Wi-Fi pairing imported; listener unavailable" : "Wi-Fi microphone not paired"
    }
    var transportPreference: DictationTransportPreference {
        DictationTransportPreference(rawValue: defaults.string(forKey: "VoiceTransport") ?? "auto") ?? .auto
    }
    var activeTransport: DictationTransport? { session.isBusy ? sessionTransport : nil }
    var wirelessSessionID: UUID? {
        guard sessionTransport == .wifi, session.phase != .idle, session.phase != .draft else { return nil }
        return wirelessSessionIdentifier
    }
    var wirelessSessionState: String {
        guard sessionTransport == .wifi else { return "idle" }
        switch session.phase {
        case .starting: return "starting"
        case .recording: return "listening"
        case .transcribing: return "stopping"
        case .error: return "error"
        default: return "idle"
        }
    }
    var ready: Bool { transportPreference.resolve(usbReady: usbReady, wifiReady: wirelessReady) != nil }
    var wireState: String {
        switch session.phase {
        case .idle, .draft: return "ready"
        case .starting: return "starting"
        case .recording: return "listening"
        case .transcribing: return "muted" // Existing firmware closes its PCM gate.
        case .error: return "error"
        }
    }
    var status: String {
        switch session.phase {
        case .idle:
            if engine == .fluidVoice && !fluidVoiceAvailable { return "Check FluidVoice in Voice Settings" }
            if transportPreference == .wifi && !wirelessReady { return "Pair the board for Wi-Fi dictation" }
            return ready ? "Dictation ready" : "Dictation needs setup"
        case .starting: return sessionTransport == .wifi ? "Starting Wi-Fi microphone" : "Starting Waveshare microphone"
        case .recording: return sessionTransport == .wifi ? "Recording over Wi-Fi" : "Recording from Waveshare"
        case .transcribing: return sessionEngine == .fluidVoice ? "Transcribing with FluidVoice" : "Finishing transcription"
        case .draft: return "Dictation draft ready"
        case .error: return session.error ?? "Dictation failed"
        }
    }

    func requestPermissions() async {
        if transportPreference != .wifi { _ = await AVCaptureDevice.requestAccess(for: .audio) }
        if engine == .appleSpeech {
            _ = await withCheckedContinuation { completion in
                SFSpeechRecognizer.requestAuthorization { completion.resume(returning: $0) }
            }
        }
        permissionRevision += 1
        onStateChange?()
    }

    func importWirelessPairing(_ data: Data) throws {
        guard let wirelessServer else { throw WirelessMicrophoneServerError.pairingNotConfigured }
        _ = try wirelessServer.importPairingBundle(data)
        try wirelessServer.start()
        permissionRevision += 1
        onStateChange?()
    }

    func removeWirelessPairing() {
        wirelessServer?.removePairing()
        permissionRevision += 1
        onStateChange?()
    }

    /// Wake Voice Settings when the listener moves between starting, ready,
    /// and failed states without changing the dictation session itself.
    func refreshWirelessReadiness() {
        permissionRevision += 1
        onStateChange?()
    }

    private var sessionTransport: DictationTransport = .usb
    // The board's server-issued session UUID is distinct from the local
    // DictationSession generation UUID used to reject late Speech callbacks.
    private var wirelessSessionIdentifier: UUID?

    func start(threadId: String) async throws {
        guard let transport = transportPreference.resolve(usbReady: usbReady, wifiReady: wirelessReady) else {
            throw DictationError.message("No selected microphone transport is ready. Connect USB or pair the board for Wi-Fi.")
        }
        guard transport == .usb else {
            throw DictationError.message("Wi-Fi dictation is started by the paired board.")
        }
        try await start(threadId: threadId, transport: transport, wirelessSessionID: nil)
    }

    func start(threadId: String, transport: DictationTransport, wirelessSessionID: UUID? = nil) async throws {
        guard UUID(uuidString: threadId) != nil else { throw DictationError.message("Dictation requires a local Codex task ID.") }
        guard !isOpeningDraft else { throw DictationError.message("Wait for the current dictation to finish opening in Codex.") }
        guard !session.isBusy else { throw DictationError.message("Finish the current dictation before starting another one.") }
        guard transport == .usb ? usbReady : (wirelessReady && wirelessSessionID != nil) else {
            throw DictationError.message(engine == .fluidVoice
                ? "Check FluidVoice in Voice Settings, then connect USB or pair the Wi-Fi microphone."
                : (transport == .usb
                    ? "Open Voice Settings and enable Microphone and Speech Recognition, then connect the board by USB."
                    : "Pair the board, allow Speech Recognition, and confirm the Mac listener is ready."))
        }
        guard sessionEngine != .fluidVoice || session.phase != .draft else {
            throw DictationError.message("Open or discard the FluidVoice draft before recording again.")
        }
        // A successful handoff clears this automatically. If a handoff failed,
        // starting a new device recording is the explicit replacement action now
        // that the review window has no Discard button.
        if !draftText.isEmpty || session.phase == .draft || session.phase == .error {
            session.discard()
            draftText = ""
        }
        let id = try session.begin(threadId: threadId)
        sessionTransport = transport
        sessionEngine = engine
        wirelessSessionIdentifier = transport == .wifi ? wirelessSessionID : nil
        handoffMessage = nil
        draftText = ""
        // Keep the editable review window out of the way while the device is
        // recording. The compact overlay is non-activating and follows the
        // same centered-bottom placement as FluidVoice.
        window?.orderOut(nil)
        DictationRecordingOverlayController.shared.show(model: self)
        onStateChange?()
        do {
            try await recorder.start(id: id, transport: transport, wirelessSessionID: wirelessSessionID,
                                     engine: sessionEngine, fluidVoicePort: fluidVoicePort) { [weak self] event in
                Task { @MainActor [weak self] in self?.handle(id: id, event: event) }
            }
            if transport == .usb { session.recording(id) }
            onStateChange?()
        } catch {
            session.fail(id, error.localizedDescription)
            DictationRecordingOverlayController.shared.hide()
            showWindow()
            onStateChange?()
            throw DictationError.message(session.error ?? error.localizedDescription)
        }
    }

    func finish(threadId: String) throws {
        guard session.threadId == threadId else { throw DictationError.message("This recording belongs to a different task.") }
        if let id = session.id, session.isBusy {
            if sessionTransport == .wifi { cancelOwnedWirelessSession() }
            session.finishing(id)
            recorder.finish(id: id)
            onStateChange?()
        }
    }

    private func cancelOwnedWirelessSession() {
        guard sessionTransport == .wifi, let id = wirelessSessionIdentifier else { return }
        wirelessServer?.cancelActiveSession(sessionID: id)
    }

    func finishWireless(threadId: String, sessionID: UUID) async -> Bool {
        guard session.threadId == threadId, sessionTransport == .wifi,
              wirelessSessionIdentifier == sessionID, let id = session.id, session.isBusy else { return false }
        session.finishing(id)
        onStateChange?()
        return await recorder.finishWireless(sessionID: sessionID)
    }

    func failWirelessSession(threadId: String, sessionID: UUID, message: String) {
        guard session.threadId == threadId,
              sessionTransport == .wifi,
              wirelessSessionIdentifier == sessionID,
              session.isBusy else { return }
        recorder.cancel(message: message, sessionID: sessionID)
    }

    /// Network callbacks call this through the recorder's private serial queue;
    /// no actor/UI state is touched here.
    @discardableResult
    nonisolated func appendWirelessFrame(_ frame: WirelessMicrophoneProtocol.AudioFrame) -> Bool {
        recorder.appendWirelessFrame(frame)
    }

    func handle(id: UUID, event: DictationRecorder.Event) {
        guard session.id == id else { return }
        if case .failed = event, sessionEngine == .fluidVoice {
            Task { await refreshFluidVoice() }
        }
        guard session.isBusy else { return }
        let previousPhase = session.phase
        switch event {
        case .prepared:
            // Wi-Fi start is acknowledged when the Speech receiver is ready;
            // the first validated PCM frame transitions the visible session to
            // recording/listening.
            DictationRecordingOverlayController.shared.show(model: self)
        case .recording:
            session.recording(id)
            DictationRecordingOverlayController.shared.show(model: self)
        case .finishing:
            if sessionTransport == .wifi { cancelOwnedWirelessSession() }
            session.finishing(id)
            DictationRecordingOverlayController.shared.show(model: self)
        case let .transcript(text, final):
            // Once review begins, late recognition callbacks must not replace
            // the editable draft with the recognizer's older copy.
            guard session.update(id, text: text, final: final) else { return }
            if final, text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty, !session.text.isEmpty {
                DictationDiagnostics.record("empty-final-kept-partial")
            }
            draftText = session.text
            if final {
                // Speech can finish naturally without a preceding finish event.
                // Stop the board stream before handing off or clearing its ID.
                if sessionTransport == .wifi { cancelOwnedWirelessSession() }
                level = 0
                DictationRecordingOverlayController.shared.hide()
                // update rejects terminal callbacks, so only the first completed
                // transcript opens the original task's composer.
                if session.phase == .draft && sessionEngine == .appleSpeech {
                    openDraft()
                }
                else { showWindow() }
            }
        case let .failed(message):
            if sessionTransport == .wifi { cancelOwnedWirelessSession() }
            session.fail(id, message)
            level = 0
            DictationRecordingOverlayController.shared.hide()
            showWindow()
        case let .level(value): level = value
        }
        if previousPhase != session.phase { DictationDiagnostics.record(session.phase.rawValue) }
        onStateChange?()
    }

    func cancelRecording() {
        guard let id = session.id, session.isBusy else { return }
        cancelOwnedWirelessSession()
        recorder.cancel(id: id)
        // Invalidate the UI generation immediately so a queued final result
        // cannot beat recorder cancellation and become a draft.
        session.fail(id, "Recording cancelled.")
        level = 0
        DictationRecordingOverlayController.shared.hide()
        onStateChange?()
        if sessionEngine == .fluidVoice {
            fluidVoiceAvailable = false
            fluidVoiceStatus = "Recording cancelled. Check FluidVoice before recording again."
        }
    }

    func discardDraft() {
        guard !session.isBusy, !isOpeningDraft else { return }
        session.discard()
        draftText = ""
        handoffMessage = nil
        wirelessSessionIdentifier = nil
        onStateChange?()
    }

    func openDraft() {
        guard !isOpeningDraft else { return }
        guard !session.isBusy, let threadId = session.threadId,
              let url = DictationDraftLink.url(threadId: threadId, text: draftText) else {
            handoffMessage = "The Codex handoff could not be prepared."
            showWindow()
            return
        }
        let sessionId = session.id
        lastAttemptedHandoff = HandoffRecovery(threadID: threadId, text: draftText)
        isOpeningDraft = true
        handoffMessage = "Opening the recorded task's draft…"
        Task {
            defer { isOpeningDraft = false }
            do {
                try await openDraftLink(url)
                guard session.id == sessionId else { return }
                DictationDiagnostics.record("draft-link-open-request-accepted")
                // Release the completed recording, but retain the recovery copy
                // until explicitly cleared or replaced by the next handoff.
                session.discard()
                draftText = ""
                sessionTransport = .usb
                wirelessSessionIdentifier = nil
                handoffMessage = nil
                window?.orderOut(nil)
                onStateChange?()
            } catch {
                guard session.id == sessionId else { return }
                handoffMessage = "Codex could not open the draft. The transcript is still here for review."
                DictationDiagnostics.record("draft-link-failed")
                showWindow()
                onStateChange?()
            }
        }
    }

    func clearHandoffRecovery() { lastAttemptedHandoff = nil }

    func showWindow() {
        DictationRecordingOverlayController.shared.hide()
        if window == nil {
            let window = NSWindow(contentViewController: NSHostingController(rootView: DictationReviewView(model: self)))
            window.title = "Device Dictation"
            window.styleMask = [.titled, .closable, .miniaturizable, .resizable]
            window.isReleasedWhenClosed = false
            window.center()
            self.window = window
        }
        NSApplication.shared.activate(ignoringOtherApps: true)
        window?.makeKeyAndOrderFront(nil)
    }
}

private struct DictationReviewView: View {
    @ObservedObject var model: DictationModel
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(model.status).font(.headline)
            if let id = model.session.threadId {
                Text("Task: \(id)").font(.caption).textSelection(.enabled)
            }
            if model.session.isBusy {
                ProgressView(value: Double(model.level)).accessibilityLabel("Waveshare microphone input level")
                Text("Long press the device button again to finish. Recording stops after 55 seconds.").font(.caption)
            }
            TextEditor(text: $model.draftText).font(.body)
                .disabled(model.session.isBusy).accessibilityLabel("Dictation text")
            if model.session.isBusy {
                Button("Cancel recording", role: .cancel) { model.cancelRecording() }
            } else if model.sessionEngine == .fluidVoice && model.session.phase == .draft {
                HStack {
                    Button("Open as Task Draft") { model.openDraft() }
                        .disabled(model.isOpeningDraft || model.draftText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    Button("Discard", role: .destructive) { model.discardDraft() }.disabled(model.isOpeningDraft)
                }
            }
            if let error = model.session.error { Text(error).foregroundStyle(.red) }
            if let message = model.handoffMessage { Text(message).font(.caption) }
            if let recovery = model.lastAttemptedHandoff {
                DisclosureGroup("Last handoff recovery copy (delivery not confirmed)") {
                    Text("Task: \(recovery.threadID)").font(.caption).textSelection(.enabled)
                    Text(recovery.text).textSelection(.enabled)
                    Button("Clear recovery copy") { model.clearHandoffRecovery() }
                }
            }
            if model.sessionEngine == .fluidVoice {
                Text("Review the text before choosing Open as Task Draft. This replaces text in the original task's composer, but never sends a message.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            if model.sessionEngine == .appleSpeech {
                Text("Native dictation requests insertion into the recorded task's Codex composer, replacing existing text. It never sends a message. Check the exact task before repeating a handoff; opening a link does not confirm composer delivery.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .padding(20).frame(minWidth: 560, minHeight: 390)
    }
}
