// SPDX-License-Identifier: MPL-2.0
import Foundation
import Network

// Only typed values cross the helper boundary. The extension bootstrap receives
// a port and random token, never an NSXPC endpoint requiring decoder exceptions.
struct JITIPCMessage: Codable, Sendable {
    var action: String
    var token: String = ""
    var pid: Int32? = nil
    var pairing: Data? = nil
    var ok: Bool? = nil
    var text: String? = nil
    var stage: String? = nil
    var fraction: Double? = nil
    var txm: Bool? = nil
    var rsdPort: UInt16? = nil
    var deviceAddress: String? = nil
}

final class JITIPCChannel: @unchecked Sendable {
    private let queue = DispatchQueue(label: "77U.JIT.helper-ipc")
    private let connection: NWConnection
    private let token: String
    private let message: @Sendable (JITIPCMessage) -> Void
    private let ended: @Sendable (String) -> Void
    private var stopped = false
    private static let maximumFrame = 4 * 1024 * 1024

    init(connection: NWConnection, token: String,
         message: @escaping @Sendable (JITIPCMessage) -> Void,
         ended: @escaping @Sendable (String) -> Void) {
        self.connection = connection
        self.token = token
        self.message = message
        self.ended = ended
    }

    func start() {
        connection.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .ready: self.readHeader()
            case .failed(let error): self.stop(error.localizedDescription)
            default: break
            }
        }
        connection.start(queue: queue)
    }

    func send(_ value: JITIPCMessage) {
        queue.async { [self] in
            guard !stopped else { return }
            var value = value
            value.token = token
            guard let payload = try? JSONEncoder().encode(value),
                  !payload.isEmpty, payload.count <= Self.maximumFrame else {
                stop("Invalid JIT helper message size."); return
            }
            var size = UInt32(payload.count).bigEndian
            var frame = withUnsafeBytes(of: &size) { Data($0) }
            frame.append(payload)
            connection.send(content: frame, completion: .contentProcessed { [weak self] error in
                if let error { self?.stop(error.localizedDescription) }
            })
        }
    }

    func close() { queue.async { self.stop(nil) } }

    private func readHeader() {
        connection.receive(minimumIncompleteLength: 4, maximumLength: 4) { [weak self] data, _, complete, error in
            guard let self, !self.stopped else { return }
            guard error == nil, let data, data.count == 4 else {
                self.stop(error?.localizedDescription ?? "JIT helper connection ended."); return
            }
            let size = data.reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
            guard size > 0, size <= Self.maximumFrame else { self.stop("Invalid JIT helper frame."); return }
            self.readPayload(Int(size))
        }
    }

    private func readPayload(_ size: Int) {
        connection.receive(minimumIncompleteLength: size, maximumLength: size) { [weak self] data, _, complete, error in
            guard let self, !self.stopped else { return }
            guard error == nil, let data, data.count == size,
                  let value = try? JSONDecoder().decode(JITIPCMessage.self, from: data),
                  value.token == self.token else {
                self.stop("Invalid or unauthenticated JIT helper message."); return
            }
            self.message(value)
            self.readHeader()
        }
    }

    private func stop(_ reason: String?) {
        dispatchPrecondition(condition: .onQueue(queue))
        guard !stopped else { return }
        stopped = true
        connection.stateUpdateHandler = nil
        connection.cancel()
        if let reason { ended(reason) }
    }
}
