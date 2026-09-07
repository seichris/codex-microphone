import AVFoundation
import Speech
import SwiftUI
import UniformTypeIdentifiers

struct VoiceSettingsView: View {
    @ObservedObject var dictation: DictationModel
    @AppStorage("VoiceDeepLinkTemplate") private var deepLink = "codex://threads/{threadId}"
    @AppStorage("VoiceTransport") private var transport = DictationTransportPreference.auto.rawValue
    @State private var requesting = false
    @State private var microphoneConnected = DictationRecorder.device != nil
    @State private var showingPairingImporter = false
    @State private var confirmingPairingRemoval = false
    @State private var pairingMessage: String?

    var body: some View {
        Form {
            Section("Device dictation") {
                Text("Long press any button to record. Long press again to end recording and open the text in Codex.")
                Text("English transcription runs on this Mac. Audio is not saved or sent to a transcription service.")
                    .font(.caption).foregroundStyle(.secondary)
                Picker("Microphone transport", selection: $transport) {
                    ForEach(DictationTransportPreference.allCases) { option in
                        Text(option.title).tag(option.rawValue)
                    }
                }
                TextField("Task deep link", text: $deepLink)
            }
            Section("Readiness") {
                readiness(microphoneConnected ? "USB microphone connected" : "USB microphone not detected",
                          ok: microphoneConnected)
                readiness(dictation.wirelessReadinessTitle,
                          ok: dictation.wirelessReady)
                readiness("USB Microphone permission", ok: AVCaptureDevice.authorizationStatus(for: .audio) == .authorized)
                readiness("Speech Recognition permission", ok: SFSpeechRecognizer.authorizationStatus() == .authorized)
                readiness("On-device English recognition", ok: DictationRecorder.onDeviceAvailable)
                Text("Wi-Fi mode requires a paired board and the local TLS listener. Pairing material is kept in Keychain; it is never written to logs.")
                    .font(.caption).foregroundStyle(.secondary)
                Button("Import Wi-Fi pairing bundle…") { showingPairingImporter = true }
                if dictation.wirelessPairingConfigured {
                    Button("Remove Wi-Fi pairing", role: .destructive) { confirmingPairingRemoval = true }
                }
                if let pairingMessage {
                    Text(pairingMessage).font(.caption).foregroundStyle(.orange)
                }
                Button(requesting ? "Requesting permissions…" : "Enable Dictation Permissions") {
                    requesting = true
                    Task { await dictation.requestPermissions(); requesting = false }
                }.disabled(requesting)
                Button("Open Dictation") { dictation.showWindow() }
            }
        }
        .formStyle(.grouped).padding().frame(width: 550, height: 560)
        .onAppear { microphoneConnected = DictationRecorder.device != nil }
        // Stable AVFoundation notification names also compile with the older
        // macOS SDK used by CI, before the Swift member names were introduced.
        .onReceive(NotificationCenter.default.publisher(for: Notification.Name("AVCaptureDeviceWasConnectedNotification"))) { _ in
            microphoneConnected = DictationRecorder.device != nil
        }
        .onReceive(NotificationCenter.default.publisher(for: Notification.Name("AVCaptureDeviceWasDisconnectedNotification"))) { _ in
            microphoneConnected = DictationRecorder.device != nil
        }
        .fileImporter(isPresented: $showingPairingImporter, allowedContentTypes: [.json], allowsMultipleSelection: false) { result in
            do {
                let url = try result.get().first
                guard let url else { return }
                let accessing = url.startAccessingSecurityScopedResource()
                defer { if accessing { url.stopAccessingSecurityScopedResource() } }
                let values = try url.resourceValues(forKeys: [.fileSizeKey])
                guard values.fileSize.map({ $0 <= 64 * 1024 }) ?? false else {
                    throw WirelessMicrophoneServerError.invalidPairing
                }
                let data = try Data(contentsOf: url, options: [.uncached])
                try dictation.importWirelessPairing(data)
                pairingMessage = nil
            } catch {
                pairingMessage = "Pairing failed: \(error.localizedDescription)"
            }
        }
        .confirmationDialog("Remove the Wi-Fi microphone pairing?", isPresented: $confirmingPairingRemoval) {
            Button("Remove Pairing", role: .destructive) { dictation.removeWirelessPairing() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("The listener will stop and the Keychain credential and TLS identity will be revoked on this Mac.")
        }
    }
    private func readiness(_ title: String, ok: Bool) -> some View {
        Label(title, systemImage: ok ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
            .foregroundStyle(ok ? .green : .orange)
    }
}
