import AppKit
import Darwin
import Foundation

/// Read-only client of the existing Codex broker. Never creates a server, claims
/// task ownership, requests task contents, or sends turn-control methods.
final class TaskEventConnection {
    typealias Update = (FocusedTaskSelection, FocusedTaskDiagnostic) -> Void
    private let lock = NSLock()
    private var stopped = false
    private let queue = DispatchQueue(label: "display.task-events", qos: .utility)
    private enum Failure: Error { case disconnected, untrustedPeer, timeout, protocolMismatch }

    func start(update: @escaping Update) {
        queue.async { [self] in
            while !isStopped {
                do { try observe(update: update) }
                catch {
                    let reason: String
                    switch error {
                    case Failure.untrustedPeer: reason = "events-untrusted"
                    case Failure.timeout: reason = "events-timeout"
                    case Failure.protocolMismatch, is TaskEventError, is DecodingError: reason = "events-incompatible"
                    default: reason = "events-disconnected"
                    }
                    update(.unavailable(), FocusedTaskDiagnostic(reason: reason))
                }
                for _ in 0..<4 where !isStopped { Thread.sleep(forTimeInterval: 0.25) }
            }
        }
    }
    func stop() { lock.lock(); stopped = true; lock.unlock() }
    private var isStopped: Bool { lock.lock(); defer { lock.unlock() }; return stopped }

    private func observe(update: Update) throws {
        let path = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".codex/ipc/ipc.sock")
        for (url, kind) in [(path.deletingLastPathComponent(), mode_t(S_IFDIR)), (path, mode_t(S_IFSOCK))] {
            var info = stat()
            guard lstat(url.path, &info) == 0 else { throw Failure.disconnected }
            guard info.st_uid == getuid(), info.st_mode & mode_t(S_IFMT) == kind,
                  info.st_mode & 0o022 == 0 else { throw Failure.untrustedPeer }
        }
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw Failure.disconnected }
        defer { close(fd) }
        var one: Int32 = 1
        guard setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout.size(ofValue: one))) == 0,
              fcntl(fd, F_SETFL, O_NONBLOCK) == 0 else { throw Failure.disconnected }
        var address = sockaddr_un()
        address.sun_family = sa_family_t(AF_UNIX)
        let bytes = Array(path.path.utf8) + [UInt8(0)]
        guard bytes.count <= MemoryLayout.size(ofValue: address.sun_path) else { throw Failure.untrustedPeer }
        withUnsafeMutableBytes(of: &address.sun_path) { $0.copyBytes(from: bytes) }
        address.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
        let connected = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
        }
        if connected != 0 {
            guard errno == EINPROGRESS else { throw Failure.disconnected }
            try wait(fd, events: Int16(POLLOUT), milliseconds: 1000)
            var socketError: Int32 = 0
            var size = socklen_t(MemoryLayout.size(ofValue: socketError))
            guard getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &size) == 0, socketError == 0 else { throw Failure.disconnected }
        }
        var uid: uid_t = 0, gid: gid_t = 0, pid: pid_t = 0
        var pidSize = socklen_t(MemoryLayout.size(ofValue: pid))
        guard getpeereid(fd, &uid, &gid) == 0, uid == getuid(),
              getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &pidSize) == 0,
              let peer = NSRunningApplication(processIdentifier: pid),
              peer.bundleIdentifier == "com.openai.codex", !peer.isTerminated else { throw Failure.untrustedPeer }

        var state = TaskEventState()
        var frames = TaskEventFrames()
        var clientId: String?
        var pendingID: String?
        var pingSent = Date.distantPast
        var lastAcknowledged = Date.distantPast
        var receiveBuffer = [UInt8](repeating: 0, count: 65_536)
        while !isStopped {
            let now = Date()
            guard !peer.isTerminated else { throw Failure.disconnected }
            if pendingID != nil && now.timeIntervalSince(pingSent) > 2 { throw Failure.timeout }
            if pendingID == nil && now.timeIntervalSince(lastAcknowledged) >= 2 {
                let id = UUID().uuidString
                pendingID = id
                pingSent = now
                // Reinitialization is an idempotent broker liveness check.
                try send(fd, ["type": "request", "requestId": id,
                    "sourceClientId": clientId ?? "codex-display-observer",
                    "version": 0, "method": "initialize",
                    "params": ["clientType": "codex-display-observer"]])
            }
            var descriptor = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
            let ready = poll(&descriptor, 1, 250)
            guard ready >= 0 || errno == EINTR else { throw Failure.disconnected }
            guard descriptor.revents & Int16(POLLHUP | POLLERR | POLLNVAL) == 0 else { throw Failure.disconnected }
            if ready > 0 && descriptor.revents & Int16(POLLIN) != 0 {
                let count = recv(fd, &receiveBuffer, receiveBuffer.count, 0)
                guard count > 0 else { throw Failure.disconnected }
                for data in try frames.append(Data(receiveBuffer.prefix(count))) {
                    guard let message = try JSONSerialization.jsonObject(with: data) as? [String: Any] else { throw Failure.protocolMismatch }
                    switch message["type"] as? String {
                    case "response":
                        if let id = message["requestId"] as? String, id == pendingID {
                            guard message["method"] as? String == "initialize",
                                  message["resultType"] as? String == "success",
                                  let result = message["result"] as? [String: Any],
                                  let assigned = result["clientId"] as? String, UUID(uuidString: assigned) != nil,
                                  clientId == nil || clientId == assigned else { throw Failure.protocolMismatch }
                            clientId = assigned
                            pendingID = nil
                            lastAcknowledged = Date()
                        }
                    case "client-discovery-request":
                        guard let id = message["requestId"] as? String else { throw Failure.protocolMismatch }
                        try send(fd, ["type": "client-discovery-response", "requestId": id, "response": ["canHandle": false]])
                    case "request":
                        guard let id = message["requestId"] as? String else { throw Failure.protocolMismatch }
                        try send(fd, ["type": "response", "requestId": id, "resultType": "error", "error": "no-handler-for-request"])
                    case "broadcast": try state.apply(data)
                    default: break
                    }
                }
            }
            let result = clientId == nil ? (FocusedTaskSelection.unavailable(), "events-connecting") : state.result()
            var diagnostic = FocusedTaskDiagnostic(reason: result.1)
            diagnostic.processID = pid
            diagnostic.appCount = 1
            diagnostic.trusted = true // Verified broker peer, not AX permission.
            diagnostic.candidates = state.candidateCount
            diagnostic.uniqueCandidates = state.uniqueCandidateCount
            diagnostic.candidateTasks = state.candidateTasks
            update(result.0, diagnostic)
        }
    }

    private func wait(_ fd: Int32, events: Int16, milliseconds: Int32) throws {
        var descriptor = pollfd(fd: fd, events: events, revents: 0)
        guard poll(&descriptor, 1, milliseconds) > 0,
              descriptor.revents & events != 0,
              descriptor.revents & Int16(POLLHUP | POLLERR | POLLNVAL) == 0 else { throw Failure.timeout }
    }
    private func send(_ fd: Int32, _ object: [String: Any]) throws {
        let data = try TaskEventFrames.encode(object)
        let deadline = Date().addingTimeInterval(1)
        try data.withUnsafeBytes { bytes in
            var offset = 0
            while offset < bytes.count {
                guard Date() < deadline, !isStopped else { throw Failure.timeout }
                let count = Darwin.send(fd, bytes.baseAddress!.advanced(by: offset), bytes.count - offset, 0)
                if count > 0 { offset += count }
                else if count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    try wait(fd, events: Int16(POLLOUT), milliseconds: 250)
                } else { throw Failure.disconnected }
            }
        }
    }
}
