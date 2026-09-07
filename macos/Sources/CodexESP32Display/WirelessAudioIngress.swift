import Foundation

/// Owns bounded admission, not PCM or Speech. The lock covers admission AND
/// enqueueing: a concurrent close/barrier cannot overtake an admitted frame.
final class WirelessAudioIngress: @unchecked Sendable {
    private let lock = NSLock()
    private let queue: DispatchQueue
    private let capacity: Int
    private var sessionID: UUID?
    private var accepting = false
    private var pending = 0

    init(queue: DispatchQueue, capacity: Int = 25) {
        precondition(capacity > 0)
        self.queue = queue
        self.capacity = capacity
    }

    func open(sessionID: UUID) {
        lock.lock(); defer { lock.unlock() }
        self.sessionID = sessionID
        accepting = true
        // Do not reset pending: obsolete queued blocks still occupy memory.
    }

    @discardableResult
    func enqueue(sessionID: UUID, _ consume: @escaping @Sendable () -> Void) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard accepting, self.sessionID == sessionID, pending < capacity else { return false }
        pending += 1
        queue.async {
            defer { self.release() }
            consume()
        }
        return true
    }

    /// Closing an old session cannot revoke the successor. The optional
    /// barrier runs after every block whose admission returned true.
    @discardableResult
    func close(sessionID: UUID, barrier: (@Sendable () -> Void)? = nil) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard self.sessionID == sessionID else { return false }
        accepting = false
        if let barrier { queue.async(execute: barrier) }
        return true
    }

    private func release() {
        lock.lock(); defer { lock.unlock() }
        precondition(pending > 0)
        pending -= 1
    }
}
