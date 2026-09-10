import Foundation

enum TaskEventError: Error { case invalidFrame, incompatibleEvent, tooManyTargets }

/// Bounded four-byte little-endian framing; no task stream payloads are logged.
struct TaskEventFrames {
    static let maximumSize = 2 * 1024 * 1024
    private var buffer = Data()

    mutating func append(_ data: Data) throws -> [Data] {
        buffer.append(data)
        var frames: [Data] = []
        while buffer.count >= 4 {
            let size = buffer.prefix(4).enumerated().reduce(0) { $0 | Int($1.element) << ($1.offset * 8) }
            guard size > 0, size <= Self.maximumSize else { throw TaskEventError.invalidFrame }
            guard buffer.count >= size + 4 else { break }
            frames.append(Data(buffer.dropFirst(4).prefix(size)))
            buffer = Data(buffer.dropFirst(size + 4))
        }
        guard buffer.count <= Self.maximumSize + 4 else { throw TaskEventError.invalidFrame }
        return frames
    }

    static func encode(_ object: [String: Any]) throws -> Data {
        let body = try JSONSerialization.data(withJSONObject: object)
        guard body.count <= maximumSize else { throw TaskEventError.invalidFrame }
        var size = UInt32(body.count).littleEndian
        return withUnsafeBytes(of: &size) { Data($0) } + body
    }
}

/// Track exact host/task IDs per originating client. Multiple presented views
/// are ambiguous, including two clients presenting the same task.
struct TaskEventState {
    private struct Target: Hashable { let id: String; let host: String }
    private struct Event: Decodable {
        let sourceClientId: String?
        let version: Int?
        let params: Params
        struct Params: Decodable {
            let conversationId: String?
            let hostId: String?
            let following: Bool?
            let clientId: String?
            let status: String?
        }
    }
    private var clients: [String: Set<Target>] = [:]
    private var settleUntil: Date
    init(at now: Date = Date()) { settleUntil = now.addingTimeInterval(0.5) }

    mutating func apply(_ data: Data, at now: Date = Date()) throws {
        guard let header = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              header["type"] as? String == "broadcast",
              let method = header["method"] as? String else { return }
        guard ["thread-stream-following-changed", "client-status-changed", "ipc-connection-reset"].contains(method) else { return }
        if method == "ipc-connection-reset" { throw TaskEventError.incompatibleEvent }
        let event = try JSONDecoder().decode(Event.self, from: data)
        if method == "client-status-changed" {
            guard event.version == 0, let client = event.params.clientId,
                  UUID(uuidString: client) != nil else { throw TaskEventError.incompatibleEvent }
            if event.params.status == "disconnected" {
                clients.removeValue(forKey: client)
                settleUntil = now.addingTimeInterval(0.3)
            }
            return
        }
        guard event.version == 1, let client = event.sourceClientId, UUID(uuidString: client) != nil,
              let id = event.params.conversationId, UUID(uuidString: id) != nil,
              let host = event.params.hostId, !host.isEmpty, host.count <= 256,
              let following = event.params.following else { throw TaskEventError.incompatibleEvent }
        let target = Target(id: id.lowercased(), host: host)
        if following { clients[client, default: []].insert(target) }
        else {
            clients[client]?.remove(target)
            if clients[client]?.isEmpty == true { clients.removeValue(forKey: client) }
        }
        guard candidateCount <= 128 else { throw TaskEventError.tooManyTargets }
        settleUntil = max(settleUntil, now.addingTimeInterval(0.3))
    }

    var candidateCount: Int { clients.values.reduce(0) { $0 + $1.count } }
    var candidateTasks: [TaskCandidateDiagnostic] {
        clients.flatMap { client, targets in
            targets.map { TaskCandidateDiagnostic(threadId: $0.id, hostId: $0.host, clientId: client) }
        }.sorted {
            ($0.hostId, $0.threadId, $0.clientId) < ($1.hostId, $1.threadId, $1.clientId)
        }
    }
    func result(at now: Date = Date()) -> (FocusedTaskSelection, String) {
        guard now >= settleUntil else { return (.unavailable(at: now), "events-settling") }
        guard candidateCount <= 1 else { return (.unavailable(at: now), "events-ambiguous") }
        guard let target = clients.values.first?.first else {
            return (FocusedTaskSelection(status: .noTask, observedAt: now), "noTask")
        }
        guard target.host == "local" else {
            return (FocusedTaskSelection(status: .unsupportedHost, hostId: target.host, observedAt: now), "unsupportedHost")
        }
        return (FocusedTaskSelection(status: .confirmed, threadId: target.id, hostId: target.host, observedAt: now), "confirmed")
    }
}
