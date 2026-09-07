import AppKit
import ApplicationServices
import Combine
import Foundation

private struct DesktopIPCRequest: Decodable {
    let version: Int
    let token: String
    let ipcId: String
    let operation: String
    let threadId: String?
    let command: String?
}

private struct DesktopCapabilities: Codable {
    let desktopFocus: Bool
    let desktopVoiceHotkey: Bool
    let powerButtonLongPress: Bool
    let wirelessMicrophone: Bool
}

private struct WirelessSessionSummary: Codable {
    let sessionID: String?
    let transport: String?
    let state: String
    let revision: UInt64
    let errorCode: String?
}

private struct DesktopStatePayload: Codable {
    let threadId: String?
    let focusConfidence: String
    let voiceState: String
    let capabilities: DesktopCapabilities
    let wirelessSession: WirelessSessionSummary
}

private struct DesktopIPCResponse: Encodable {
    let version = 1
    let ipcId: String
    let ok: Bool
    let error: String?
    let message: String?
    let statusCode: Int?
    let state: DesktopStatePayload?
}

struct DesktopControlEnvironment {
    let directory: String
    let token: String
}

@MainActor
final class DesktopVoiceController: ObservableObject {
    @Published private(set) var currentThreadId: String?
    @Published private(set) var focusConfidence = "unavailable"
    @Published private(set) var voiceState = "unknown"
    @Published private(set) var lastError: String?

    let dictation: DictationModel
    let focusedTask = FocusedTaskObserver()
    let wirelessServer: WirelessMicrophoneServer

    private let fileManager = FileManager.default
    private let root: URL
    private let token: String
    private var timer: Timer?
    private var wirelessRevision: UInt64 = 0

    init() {
        wirelessServer = WirelessMicrophoneServer()
        dictation = DictationModel(wirelessServer: wirelessServer)
        root = fileManager.temporaryDirectory
            .appendingPathComponent(
                "CodexESP32Display-\(ProcessInfo.processInfo.processIdentifier)",
                isDirectory: true
            )
        token = UUID().uuidString.replacingOccurrences(of: "-", with: "")
            + UUID().uuidString.replacingOccurrences(of: "-", with: "")
        do {
            try prepareDirectories()
            wirelessServer.onStart = { [weak self] threadID, sessionID in
                guard let self else { return false }
                return await self.acceptWirelessStart(threadID: threadID, sessionID: sessionID)
            }
            wirelessServer.onStop = { [weak self] threadID, sessionID, _ in
                guard let self else { return }
                await MainActor.run {
                    guard self.dictation.wirelessSessionID == sessionID else { return }
                    try? self.dictation.finish(threadId: threadID)
                }
            }
            wirelessServer.onAudioFrame = { [weak dictation] frame in
                dictation?.appendWirelessFrame(frame)
            }
            wirelessServer.onSessionFailure = { [weak self] threadID, sessionID, reason in
                guard let self else { return }
                await MainActor.run {
                    self.dictation.failWirelessSession(threadId: threadID, sessionID: sessionID, message: reason)
                }
            }
            wirelessServer.onStateChange = { [weak self] _ in
                Task { @MainActor [weak self] in self?.dictation.refreshWirelessReadiness() }
            }
            dictation.onStateChange = { [weak self] in
                guard let self else { return }
                self.voiceState = self.dictation.wireState
                self.lastError = self.dictation.session.error
                self.wirelessRevision &+= 1
            }
            // Pairing is optional during USB-only operation. A missing
            // identity is surfaced as a setup state, not a launch failure.
            try? wirelessServer.start()
            timer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { [weak self] _ in
                Task { @MainActor [weak self] in self?.drainRequests() }
            }
        } catch {
            lastError = "Desktop control IPC: \(error.localizedDescription)"
        }
    }

    private func acceptWirelessStart(threadID: String, sessionID: UUID) async -> Bool {
        guard currentThreadId == threadID else { return false }
        do {
            try await dictation.start(threadId: threadID, transport: .wifi, wirelessSessionID: sessionID)
            return true
        } catch { return false }
    }

    deinit {
        timer?.invalidate()
        wirelessServer.stop()
        try? fileManager.removeItem(at: root)
    }

    var environment: DesktopControlEnvironment? {
        guard lastError == nil else { return nil }
        return DesktopControlEnvironment(directory: root.path, token: token)
    }

    var statusTitle: String {
        if let lastError { return "Voice control: \(lastError)" }
        return dictation.status
    }

    private var capabilities: DesktopCapabilities {
        DesktopCapabilities(
            desktopFocus: Self.codexURL != nil && Self.deepLinkTemplateIsValid,
            // Legacy wire name means the old HTTP/USB start path is safe. Do not
            // advertise it when only the new WSS transport is ready.
            desktopVoiceHotkey: dictation.usbReady,
            powerButtonLongPress: true,
            wirelessMicrophone: wirelessServer.isReady
        )
    }

    private var wirelessSessionPayload: WirelessSessionSummary {
        let sessionID = dictation.wirelessSessionID?.uuidString
        return WirelessSessionSummary(
            sessionID: sessionID,
            transport: sessionID == nil ? nil : "wifi",
            state: sessionID == nil ? "idle" : dictation.wirelessSessionState,
            revision: wirelessRevision,
            errorCode: sessionID == nil ? nil : (dictation.session.error == nil ? nil : "dictation_failed")
        )
    }

    private var statePayload: DesktopStatePayload {
        DesktopStatePayload(
            threadId: currentThreadId,
            focusConfidence: currentThreadId == nil ? "unavailable" : focusConfidence,
            voiceState: voiceState,
            capabilities: capabilities,
            wirelessSession: wirelessSessionPayload
        )
    }

    private var observedStatePayload: DesktopStatePayload {
        let selection = focusedTask.selection
        return DesktopStatePayload(
            threadId: selection.threadId,
            focusConfidence: selection.status == .confirmed ? "confirmed" : "unavailable",
            voiceState: selection.voiceState(for: dictation.session.threadId, state: dictation.wireState),
            capabilities: capabilities,
            wirelessSession: wirelessSessionPayload
        )
    }

    private func prepareDirectories() throws {
        try fileManager.createDirectory(at: root, withIntermediateDirectories: true)
        try fileManager.createDirectory(at: root.appendingPathComponent("requests"), withIntermediateDirectories: true)
        try fileManager.createDirectory(at: root.appendingPathComponent("responses"), withIntermediateDirectories: true)
        for directory in [root, root.appendingPathComponent("requests"), root.appendingPathComponent("responses")] {
            try fileManager.setAttributes([.posixPermissions: 0o700], ofItemAtPath: directory.path)
        }
    }

    private func drainRequests() {
        let requestDirectory = root.appendingPathComponent("requests")
        guard let files = try? fileManager.contentsOfDirectory(
            at: requestDirectory,
            includingPropertiesForKeys: nil,
            options: [.skipsHiddenFiles]
        ) else { return }

        for file in files.filter({ $0.pathExtension == "json" }).sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
            processRequest(at: file)
        }
    }

    private func processRequest(at url: URL) {

        let requestData = try? Data(contentsOf: url)
        try? fileManager.removeItem(at: url)
        guard let data = requestData,
              data.count <= 4096,
              let request = try? JSONDecoder().decode(DesktopIPCRequest.self, from: data),
              request.version == 1,
              request.token == token,
              request.ipcId == url.deletingPathExtension().lastPathComponent else {
            return
        }

        let response: DesktopIPCResponse
        switch request.operation {
        case "state":
            response = success(request.ipcId, observed: true)
        case "focus":
            Task {
                let response = await focus(request)
                writeResponse(response, ipcId: request.ipcId)
            }
            return
        case "voice":
            Task {
                let response = await voice(request)
                writeResponse(response, ipcId: request.ipcId)
            }
            return
        default:
            response = failure(request.ipcId, "invalid_request", "Unknown Desktop operation.", 400)
        }
        writeResponse(response, ipcId: request.ipcId)
    }

    private func focus(_ request: DesktopIPCRequest) async -> DesktopIPCResponse {
        guard let threadId = request.threadId, Self.validThreadId(threadId) else {
            return failure(request.ipcId, "invalid_request", "Invalid thread ID.", 400)
        }
        guard !dictation.session.isBusy || dictation.session.threadId == threadId else {
            return failure(request.ipcId, "dictation_busy", "Finish dictation before switching tasks.", 409)
        }
        guard Self.codexURL != nil else {
            return failure(request.ipcId, "codex_not_installed", "Codex Desktop is not installed.", 503)
        }
        let template = UserDefaults.standard.string(forKey: "VoiceDeepLinkTemplate")
            ?? "codex://threads/{threadId}"
        let encoded = threadId.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? threadId
        guard template.contains("{threadId}"),
              let url = URL(string: template.replacingOccurrences(of: "{threadId}", with: encoded)) else {
            return failure(request.ipcId, "desktop_focus_failed", "The configured task deep link could not be opened.", 503)
        }

        do { try await CodexAppLink.open(url) }
        catch { return failure(request.ipcId, "desktop_focus_failed", error.localizedDescription, 503) }

        if currentThreadId != threadId {
            voiceState = "muted"
        }
        currentThreadId = threadId
        // Opening an exact-ID deep link is deterministic, but the public Desktop
        // surface does not expose a selected-thread acknowledgement.
        focusConfidence = "inferred"
        lastError = nil
        return success(request.ipcId)
    }

    private func voice(_ request: DesktopIPCRequest) async -> DesktopIPCResponse {
        guard let threadId = request.threadId,
              Self.validThreadId(threadId), threadId == currentThreadId else {
            return failure(request.ipcId, "desktop_thread_not_focused", "Focus this task before dictating.", 409)
        }
        do {
            switch request.command {
            case "mute": try dictation.finish(threadId: threadId)
            case "start-or-resume": try await dictation.start(threadId: threadId)
            default: return failure(request.ipcId, "invalid_request", "Unknown dictation command.", 400)
            }
            voiceState = dictation.wireState
            lastError = nil
            return success(request.ipcId)
        } catch {
            return failure(request.ipcId, "dictation_failed", error.localizedDescription, 503)
        }
    }

    private func success(_ ipcId: String, observed: Bool = false) -> DesktopIPCResponse {
        DesktopIPCResponse(
            ipcId: ipcId,
            ok: true,
            error: nil,
            message: nil,
            statusCode: nil,
            state: observed ? observedStatePayload : statePayload
        )
    }

    private func failure(_ ipcId: String, _ error: String, _ message: String, _ statusCode: Int) -> DesktopIPCResponse {
        lastError = message
        voiceState = "error"
        return DesktopIPCResponse(
            ipcId: ipcId,
            ok: false,
            error: error,
            message: message,
            statusCode: statusCode,
            state: statePayload
        )
    }

    private func writeResponse(_ response: DesktopIPCResponse, ipcId: String) {
        let directory = root.appendingPathComponent("responses")
        let destination = directory.appendingPathComponent("\(ipcId).json")
        let temporary = directory.appendingPathComponent(".\(ipcId).tmp")
        guard let data = try? JSONEncoder().encode(response) else { return }
        do {
            try data.write(to: temporary, options: .atomic)
            try fileManager.setAttributes([.posixPermissions: 0o600], ofItemAtPath: temporary.path)
            if fileManager.fileExists(atPath: destination.path) {
                try fileManager.removeItem(at: destination)
            }
            try fileManager.moveItem(at: temporary, to: destination)
        } catch {
            try? fileManager.removeItem(at: temporary)
        }
    }

    static var configuredShortcut: VoiceShortcut? {
        VoiceShortcut.parse(UserDefaults.standard.string(forKey: "VoiceShortcut") ?? "control+option+space")
    }

    static var deepLinkTemplateIsValid: Bool {
        let template = UserDefaults.standard.string(forKey: "VoiceDeepLinkTemplate")
            ?? "codex://threads/{threadId}"
        return template.contains("{threadId}")
            && URL(string: template.replacingOccurrences(of: "{threadId}", with: "thread-id")) != nil
    }

    static var codexURL: URL? {
        CodexAppLink.applicationURL
    }

    static func validThreadId(_ value: String) -> Bool {
        value.count >= 16
            && value.count <= 160
            && value.range(of: "^[A-Za-z0-9_-]+$", options: .regularExpression) != nil
    }
}
