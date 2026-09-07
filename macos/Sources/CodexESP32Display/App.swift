import AppKit
import SwiftUI

@MainActor
final class CodexESP32DisplayAppDelegate: NSObject, NSApplicationDelegate {
    private let bridge = BridgeController()
    private var statusItemController: StatusItemController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItemController = StatusItemController(bridge: bridge)
        let preference = DictationTransportPreference(
            rawValue: UserDefaults.standard.string(forKey: "VoiceTransport") ?? "auto"
        ) ?? .auto
        let wifiPaired = WirelessPairingStore.shared.pairing != nil
        let usbPermissionRequired = preference == .usb || (preference == .auto && !wifiPaired)
        if !DictationRecorder.speechPermissionReady || !DictationRecorder.onDeviceAvailable
            || (usbPermissionRequired && !DictationRecorder.microphonePermissionReady) {
            bridge.openVoiceSettings()
        }
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        bridge.openVoiceSettings()
        return true
    }

    func applicationWillTerminate(_ notification: Notification) {
        statusItemController?.invalidate()
        BridgeController.active?.stop()
    }
}

@main
struct CodexESP32DisplayApp: App {
    @NSApplicationDelegateAdaptor(CodexESP32DisplayAppDelegate.self) private var appDelegate

    var body: some Scene {
        Settings {
            if let dictation = BridgeController.active?.desktopVoiceController.dictation {
                VoiceSettingsView(dictation: dictation)
            }
        }
    }
}
