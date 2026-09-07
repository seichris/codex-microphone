import Foundation

/// Authenticated idle sockets must not retain the single-board slot forever
/// after a power cycle. All state and callbacks run on the server queue.
final class WirelessConnectionHeartbeat: @unchecked Sendable {
    typealias Reply = @Sendable (Bool) -> Void
    private let queue: DispatchQueue
    private let interval: TimeInterval
    private let timeout: TimeInterval
    private let ping: @Sendable (@escaping Reply) -> Void
    private let failure: @Sendable () -> Void
    private var running = false
    private var pending: UUID?
    private var scheduled: DispatchWorkItem?

    init(queue: DispatchQueue, interval: TimeInterval = 5, timeout: TimeInterval = 10,
         ping: @escaping @Sendable (@escaping Reply) -> Void,
         failure: @escaping @Sendable () -> Void) {
        self.queue = queue
        self.interval = interval
        self.timeout = timeout
        self.ping = ping
        self.failure = failure
    }

    func start() {
        queue.async {
            guard !self.running else { return }
            self.running = true
            self.scheduleProbe()
        }
    }

    func stop() {
        queue.async {
            self.running = false
            self.pending = nil
            self.scheduled?.cancel()
            self.scheduled = nil
        }
    }

    private func scheduleProbe() {
        let work = DispatchWorkItem { [weak self] in self?.probe() }
        scheduled = work
        queue.asyncAfter(deadline: .now() + interval, execute: work)
    }

    private func probe() {
        guard running else { return }
        let id = UUID()
        pending = id
        let deadline = DispatchWorkItem { [weak self] in self?.complete(id, alive: false) }
        scheduled = deadline
        queue.asyncAfter(deadline: .now() + timeout, execute: deadline)
        ping { [weak self] alive in
            guard let self else { return }
            self.queue.async { self.complete(id, alive: alive) }
        }
    }

    private func complete(_ id: UUID, alive: Bool) {
        guard running, pending == id else { return }
        pending = nil
        scheduled?.cancel()
        scheduled = nil
        if alive { scheduleProbe() }
        else {
            running = false
            failure()
        }
    }
}
