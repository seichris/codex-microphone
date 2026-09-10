import Foundation

/// Local diagnostics include task/client IDs, but never task contents or credentials.
struct TaskCandidateDiagnostic: Equatable, Codable {
    let threadId: String
    let hostId: String
    let clientId: String
}

struct FocusedTaskDiagnostic: Equatable, Codable {
    var reason: String
    var appCount = 0
    var processID: Int32 = 0
    var trusted = false
    var accessibilitySetup: Int32?
    var axErrors: [String: Int32] = [:]
    var visited = 0
    var webAreas = 0
    var candidates = 0
    var candidateTasks: [TaskCandidateDiagnostic] = []
    var elapsedMilliseconds = 0

    var message: String {
        switch reason {
        case "events-connecting", "events-settling": return "Detecting the open Codex task…"
        case "events-disconnected": return "Waiting for Codex’s local connection"
        case "events-timeout": return "Codex’s local connection stopped responding; reconnecting"
        case "events-untrusted": return "Could not verify Codex’s local connection"
        case "events-incompatible": return "This Codex version’s task events are not supported"
        case "events-ambiguous": return "Multiple task views are open; current task is ambiguous"
        case "permission-needed": return "Accessibility permission needed"
        case "app-not-running": return "Open Codex to detect its current task"
        case "ambiguous-app": return "Multiple Codex copies are running; focus the one to use"
        case "window-unavailable": return "Codex did not expose a focused or main window"
        case "window-minimized": return "The Codex window is minimized"
        case "shell-document": return "Codex’s app page does not expose the current task"
        case "document-unavailable": return "Codex did not expose its task URL"
        case "multiple-documents": return "Multiple document URLs were exposed; selection is ambiguous"
        case "read-failed": return "The Codex accessibility read failed; see the detection log"
        case "time-limit": return "Reading the Codex window timed out"
        case "tree-limit": return "The Codex window exceeded the bounded accessibility scan"
        case "stale": return "Task detection is waiting for a fresh window reading"
        case "confirmed": return "Current task detected"
        case "noTask": return "No open task announced by Codex"
        case "unsupportedHost": return "Remote task detection is not supported yet"
        default: return "The exposed document URL is not a supported task route"
        }
    }
}

enum FocusedTaskDiagnostics {
    static let fileURL = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Logs/CodexESP32Display/focused-task.log")
    private static let queue = DispatchQueue(label: "display.focus-diagnostics")
    private static var previous: FocusedTaskDiagnostic?
    private static var lastWrite = Date.distantPast

    static func record(_ diagnostic: FocusedTaskDiagnostic) {
        queue.async {
            var fingerprint = diagnostic
            fingerprint.elapsedMilliseconds = 0
            let now = Date()
            guard fingerprint != previous || now.timeIntervalSince(lastWrite) >= 30 else { return }
            previous = fingerprint
            lastWrite = now
            let manager = FileManager.default
            do {
                try manager.createDirectory(at: fileURL.deletingLastPathComponent(), withIntermediateDirectories: true)
                // Keep at most two small files, including across app launches.
                if let size = try? manager.attributesOfItem(atPath: fileURL.path)[.size] as? NSNumber,
                   size.intValue >= 262_144 {
                    let old = fileURL.appendingPathExtension("previous")
                    if manager.fileExists(atPath: old.path) { try manager.removeItem(at: old) }
                    try manager.moveItem(at: fileURL, to: old)
                }
                if !manager.fileExists(atPath: fileURL.path) {
                    manager.createFile(atPath: fileURL.path, contents: nil, attributes: [.posixPermissions: 0o600])
                }
                try manager.setAttributes([.posixPermissions: 0o600], ofItemAtPath: fileURL.path)
                let handle = try FileHandle(forWritingTo: fileURL)
                defer { try? handle.close() }
                let encoder = JSONEncoder()
                encoder.outputFormatting = [.sortedKeys]
                let metadata = String(decoding: try encoder.encode(diagnostic), as: UTF8.self)
                try handle.seekToEnd()
                try handle.write(contentsOf: Data("\(ISO8601DateFormatter().string(from: now)) \(metadata)\n".utf8))
            } catch { /* Diagnostics must not interrupt dictation or bridge IPC. */ }
        }
    }
}
