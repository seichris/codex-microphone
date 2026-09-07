import AppKit
import Foundation

@MainActor
final class StatusItemController: NSObject {
    private let bridge: BridgeController
    private let statusItem: NSStatusItem
    private let menu: NSMenu
    private var updateTimer: Timer?

    private let stateItem = NSMenuItem()
    private let healthItem = NSMenuItem()
    private let voiceItem = NSMenuItem()
    private let toggleItem = NSMenuItem()
    private let dashboardItem = NSMenuItem()

    init(bridge: BridgeController) {
        self.bridge = bridge
        self.statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        self.menu = NSMenu()
        super.init()

        configureStatusItem()
        configureMenu()
        updateMenu()

        updateTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in
                self?.updateMenu()
            }
        }
    }

    func invalidate() {
        updateTimer?.invalidate()
        updateTimer = nil
        statusItem.menu = nil
        NSStatusBar.system.removeStatusItem(statusItem)
    }

    private func configureStatusItem() {
        guard let button = statusItem.button else { return }
        button.image = DeviceFilledIcon.makeTemplateImage()
        button.imagePosition = .imageOnly
        button.imageScaling = .scaleProportionallyDown
        button.toolTip = "Codex ESP32 Display"
        button.setAccessibilityLabel("Codex ESP32 Display")
        statusItem.menu = menu
    }

    private func configureMenu() {
        menu.autoenablesItems = false

        stateItem.isEnabled = false
        healthItem.isEnabled = false
        menu.addItem(stateItem)
        menu.addItem(healthItem)
        voiceItem.isEnabled = false
        menu.addItem(voiceItem)
        menu.addItem(.separator())

        toggleItem.target = self
        toggleItem.action = #selector(toggleBridge)
        menu.addItem(toggleItem)

        dashboardItem.target = self
        dashboardItem.action = #selector(openDashboard)
        dashboardItem.title = "Open Dashboard"
        menu.addItem(dashboardItem)

        let copyItem = NSMenuItem(
            title: "Copy Device Endpoint",
            action: #selector(copyEndpoint),
            keyEquivalent: ""
        )
        copyItem.target = self
        menu.addItem(copyItem)

        let copyTokenItem = NSMenuItem(
            title: "Copy Bridge Token",
            action: #selector(copyToken),
            keyEquivalent: ""
        )
        copyTokenItem.target = self
        menu.addItem(copyTokenItem)

        let logsItem = NSMenuItem(
            title: "Reveal Logs",
            action: #selector(revealLogs),
            keyEquivalent: ""
        )
        logsItem.target = self
        menu.addItem(logsItem)

        let voiceSettingsItem = NSMenuItem(
            title: "Voice Settings…",
            action: #selector(openVoiceSettings),
            keyEquivalent: ","
        )
        voiceSettingsItem.target = self
        menu.addItem(voiceSettingsItem)
        let dictationItem = NSMenuItem(title: "Open Dictation…", action: #selector(openDictation), keyEquivalent: "")
        dictationItem.target = self
        menu.addItem(dictationItem)

        menu.addItem(.separator())

        let quitItem = NSMenuItem(title: "Quit", action: #selector(quit), keyEquivalent: "q")
        quitItem.target = self
        menu.addItem(quitItem)
    }

    private func updateMenu() {
        stateItem.title = bridge.state.title
        stateItem.image = NSImage(
            systemSymbolName: bridge.state.symbolName,
            accessibilityDescription: nil
        )

        healthItem.title = bridge.health.title
        healthItem.image = NSImage(
            systemSymbolName: healthSymbol,
            accessibilityDescription: nil
        )

        voiceItem.title = bridge.desktopVoiceController.statusTitle
        voiceItem.image = NSImage(
            systemSymbolName: bridge.desktopVoiceController.voiceState == "listening"
                ? "mic.fill"
                : "mic.slash",
            accessibilityDescription: nil
        )

        toggleItem.title = bridge.isRunning ? "Stop Bridge" : "Start Bridge"
        toggleItem.image = NSImage(
            systemSymbolName: bridge.isRunning ? "stop.fill" : "play.fill",
            accessibilityDescription: nil
        )

        dashboardItem.isEnabled = bridge.isRunning
        dashboardItem.image = NSImage(systemSymbolName: "safari", accessibilityDescription: nil)
    }

    private var healthSymbol: String {
        switch bridge.health {
        case .healthy(let connected):
            return connected ? "checkmark.circle" : "arrow.triangle.2.circlepath"
        case .unknown:
            return "questionmark.circle"
        case .unavailable:
            return "xmark.circle"
        }
    }

    @objc private func toggleBridge() {
        bridge.isRunning ? bridge.stop() : bridge.start()
        updateMenu()
    }

    @objc private func openDashboard() {
        bridge.openDashboard()
    }

    @objc private func copyEndpoint() {
        bridge.copyEndpoint()
    }

    @objc private func copyToken() {
        bridge.copyToken()
    }

    @objc private func revealLogs() {
        bridge.revealLogs()
    }

    @objc private func openVoiceSettings() {
        bridge.openVoiceSettings()
    }

    @objc private func openDictation() {
        bridge.desktopVoiceController.dictation.showWindow()
    }

    @objc private func quit() {
        NSApplication.shared.terminate(nil)
    }
}
