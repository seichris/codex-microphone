import AppKit
import Combine
import Foundation

enum BridgeState: Equatable {
    case stopped
    case starting
    case running
    case stopping
    case failed(String)

    var title: String {
        switch self {
        case .stopped:
            return "Bridge stopped"
        case .starting:
            return "Starting bridge"
        case .running:
            return "Bridge running"
        case .stopping:
            return "Stopping bridge"
        case .failed(let message):
            return "Bridge error: \(message)"
        }
    }

    var symbolName: String {
        switch self {
        case .running:
            return "checkmark.circle.fill"
        case .starting, .stopping:
            return "arrow.triangle.2.circlepath"
        case .failed:
            return "exclamationmark.triangle.fill"
        case .stopped:
            return "power"
        }
    }
}

enum BridgeHealth: Equatable {
    case unknown
    case healthy(appServerConnected: Bool)
    case unavailable

    var title: String {
        switch self {
        case .unknown:
            return "Checking health..."
        case .healthy(let connected):
            return connected ? "Codex connected" : "Codex reconnecting"
        case .unavailable:
            return "Health check unavailable"
        }
    }
}

@MainActor
final class BridgeController: ObservableObject {
    static weak var active: BridgeController?

    @Published private(set) var state: BridgeState = .stopped
    @Published private(set) var health: BridgeHealth = .unknown

    let endpoint = URL(string: "http://127.0.0.1:5180/api/v1/attention")!
    let dashboardURL = URL(string: "http://127.0.0.1:5180/")!
    let logURL: URL
    let desktopVoiceController = DesktopVoiceController()

    private let bridgeRoot: URL
    private var bridgeToken: String?
    private var process: Process?
    private var logHandle: FileHandle?
    private var healthTimer: Timer?
    private lazy var voiceSettingsWindow = VoiceSettingsWindowController(dictation: desktopVoiceController.dictation)

    init() {
        _ = FileManager.default.changeCurrentDirectoryPath("/tmp")
        bridgeRoot = Self.resolveBridgeRoot()
        logURL = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Logs/CodexESP32Display/bridge.log")

        Self.active = self

        start()
    }

    deinit {
        logHandle?.closeFile()
    }

    var isRunning: Bool {
        process?.isRunning == true
    }

    var bridgeRootPath: String {
        bridgeRoot.path
    }

    func start() {
        guard process == nil else { return }

        bridgeToken = Self.resolveBridgeToken(from: bridgeRoot)

        let bridgeEntry = bridgeRoot.appendingPathComponent("bridge/src/index.mjs")
        guard FileManager.default.fileExists(atPath: bridgeEntry.path) else {
            state = .failed("bridge source not found")
            return
        }

        guard let nodeURL = Self.resolveNode() else {
            state = .failed("Node.js not found")
            return
        }

        do {
            try prepareLogFile()

            let child = Process()
            child.executableURL = nodeURL
            child.arguments = [bridgeEntry.path]
            // Finder-launched apps can stall Node's getcwd in a protected workspace.
            // The bridge entry point and config path are absolute, so a neutral cwd is safe.
            child.currentDirectoryURL = URL(fileURLWithPath: "/tmp", isDirectory: true)
            child.standardOutput = logHandle
            child.standardError = logHandle

            var environment = ProcessInfo.processInfo.environment
            let pathEntries = [
                FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".local/bin").path,
                "/opt/homebrew/bin",
                "/usr/local/bin",
                "/usr/bin",
                "/bin",
            ]
            environment["PATH"] = pathEntries.joined(separator: ":")
            environment["PWD"] = "/tmp"
            environment["OLDPWD"] = "/tmp"
            environment["CODEX_ATTENTION_CONFIG"] = bridgeRoot
                .appendingPathComponent("bridge/config.json")
                .path
            if let codexURL = Self.resolveCodex() {
                environment["CODEX_BIN"] = codexURL.path
            }
            if let control = desktopVoiceController.environment {
                environment["CODEX_DESKTOP_CONTROL_DIR"] = control.directory
                environment["CODEX_DESKTOP_CONTROL_TOKEN"] = control.token
            }
            child.environment = environment
            child.terminationHandler = { [weak self] terminatedProcess in
                Task { @MainActor [weak self] in
                    self?.processDidTerminate(status: terminatedProcess.terminationStatus)
                }
            }

            state = .starting
            health = .unknown
            process = child
            try child.run()
            scheduleHealthChecks()
        } catch {
            logHandle?.closeFile()
            logHandle = nil
            process = nil
            state = .failed(error.localizedDescription)
        }
    }

    func stop() {
        guard let child = process else {
            state = .stopped
            healthTimer?.invalidate()
            healthTimer = nil
            return
        }

        state = .stopping
        healthTimer?.invalidate()
        healthTimer = nil
        if child.isRunning {
            child.terminate()
        } else {
            processDidTerminate(status: child.terminationStatus)
        }
    }

    func openDashboard() {
        // Keep administrative credentials out of URLs, browser history and
        // copied endpoints. The dashboard retains its explicit token login.
        NSWorkspace.shared.open(dashboardURL)
    }

    func copyEndpoint() {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(endpoint.absoluteString, forType: .string)
    }

    func revealLogs() {
        NSWorkspace.shared.activateFileViewerSelecting([logURL])
    }

    func openVoiceSettings() {
        voiceSettingsWindow.show()
    }

    private func processDidTerminate(status: Int32) {
        process = nil
        logHandle?.closeFile()
        logHandle = nil
        healthTimer?.invalidate()
        healthTimer = nil

        if state == .stopping || status == 0 {
            state = .stopped
        } else {
            state = .failed("process exited (\(status))")
        }
        health = .unavailable
    }

    private func scheduleHealthChecks() {
        healthTimer?.invalidate()
        healthTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in
                self?.refreshHealth()
            }
        }
        refreshHealth()
    }

    private func refreshHealth() {
        let healthURL = dashboardURL.appendingPathComponent("healthz")
        URLSession.shared.dataTask(with: healthURL) { [weak self] data, response, _ in
            let statusCode = (response as? HTTPURLResponse)?.statusCode
            let payload = data.flatMap { try? JSONDecoder().decode(HealthPayload.self, from: $0) }
            Task { @MainActor [weak self] in
                guard let self else { return }
                guard self.process != nil else {
                    self.health = .unavailable
                    return
                }
                guard statusCode == 200, let payload else {
                    self.health = .unavailable
                    return
                }
                self.health = .healthy(appServerConnected: payload.appServerConnected)
                if self.state == .starting {
                    self.state = .running
                }
            }
        }.resume()
    }

    private func prepareLogFile() throws {
        let directory = logURL.deletingLastPathComponent()
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        if !FileManager.default.fileExists(atPath: logURL.path) {
            FileManager.default.createFile(atPath: logURL.path, contents: nil)
        }
        let handle = try FileHandle(forWritingTo: logURL)
        handle.seekToEndOfFile()
        logHandle = handle
    }

    private static func resolveBridgeRoot() -> URL {
        let fileManager = FileManager.default
        if let configured = ProcessInfo.processInfo.environment["CODEX_ESP32_DISPLAY_ROOT"],
           !configured.isEmpty {
            return URL(fileURLWithPath: configured).standardizedFileURL
        }
        if let resourceRoot = Bundle.main.resourceURL?.standardizedFileURL,
           fileManager.fileExists(atPath: resourceRoot.appendingPathComponent("bridge/src/index.mjs").path) {
            return resourceRoot
        }
        if let bundled = Bundle.main.object(forInfoDictionaryKey: "BridgeRoot") as? String,
           !bundled.isEmpty,
           fileManager.fileExists(atPath: URL(fileURLWithPath: bundled).appendingPathComponent("bridge/src/index.mjs").path) {
            return URL(fileURLWithPath: bundled).standardizedFileURL
        }

        let bundleRoot = Bundle.main.bundleURL
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        if fileManager.fileExists(atPath: bundleRoot.appendingPathComponent("bridge/src/index.mjs").path) {
            return bundleRoot.standardizedFileURL
        }

        return URL(fileURLWithPath: fileManager.currentDirectoryPath).standardizedFileURL
    }

    private static func resolveBridgeToken(from bridgeRoot: URL) -> String? {
        if let environmentToken = ProcessInfo.processInfo.environment["CODEX_ATTENTION_TOKEN"] {
            return environmentToken.isEmpty ? nil : environmentToken
        }

        let configURL = bridgeRoot.appendingPathComponent("bridge/config.json")
        guard let data = try? Data(contentsOf: configURL),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let token = object["token"] as? String,
              !token.isEmpty else {
            return nil
        }
        return token
    }

    private static func resolveNode() -> URL? {
        let fileManager = FileManager.default
        var candidates = [
            "/opt/homebrew/bin/node",
            "/usr/local/bin/node",
            "/usr/bin/node",
        ]

        let nvmRoot = fileManager.homeDirectoryForCurrentUser
            .appendingPathComponent(".nvm/versions/node")
        if let versions = try? fileManager.contentsOfDirectory(at: nvmRoot, includingPropertiesForKeys: nil) {
            candidates.append(contentsOf: versions
                .sorted { $0.lastPathComponent > $1.lastPathComponent }
                .map { $0.appendingPathComponent("bin/node").path })
        }

        return candidates
            .map { URL(fileURLWithPath: $0) }
            .first { fileManager.isExecutableFile(atPath: $0.path) }
    }

    private static func resolveCodex() -> URL? {
        let fileManager = FileManager.default
        // Read metadata with the same Desktop version that owns the event
        // connection. An older PATH CLI may not understand its task storage.
        let bundled = CodexAppLink.applicationURL?.appendingPathComponent("Contents/Resources/codex").path
        let candidates = [bundled].compactMap { $0 } + [
            "/opt/homebrew/bin/codex",
            "/usr/local/bin/codex",
            fileManager.homeDirectoryForCurrentUser.appendingPathComponent(".local/bin/codex").path,
            "/Applications/ChatGPT.app/Contents/Resources/codex",
            "/Applications/Codex.app/Contents/Resources/codex",
        ]
        return candidates
            .map { URL(fileURLWithPath: $0) }
            .first { fileManager.isExecutableFile(atPath: $0.path) }
    }
}

private struct HealthPayload: Decodable {
    let appServerConnected: Bool
}
