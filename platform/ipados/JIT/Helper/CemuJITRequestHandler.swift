// SPDX-License-Identifier: MPL-2.0
import Foundation
import Network

// The extension process handles IPC commands on one serial worker. The only
// mutable references below are initialized before the channel starts receiving;
// the network callback never reads them directly.
@objc(CemuJITRequestHandler)
final class CemuJITRequestHandler: NSObject, NSExtensionRequestHandling, @unchecked Sendable {
    private let worker = DispatchQueue(label: "77U.JIT.extension-worker", qos: .userInitiated)
    private var channel: JITIPCChannel?
    private var requestContext: NSExtensionContext?
    private let diskImages: DDIPaths = {
        let library = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
        return DDIPaths.default(in: library.appendingPathComponent("StikJIT"))
    }()

    func beginRequest(with context: NSExtensionContext) {
        guard let item = context.inputItems.first as? NSExtensionItem,
              let number = item.userInfo?["77UJITPort"] as? NSNumber,
              let port = NWEndpoint.Port(rawValue: number.uint16Value),
              let token = item.userInfo?["77UJITToken"] as? String,
              token.count == 72 else {
            context.cancelRequest(withError: NSError(domain: "77U.JIT", code: 10))
            return
        }

        let connection = NWConnection(host: "127.0.0.1", port: port, using: .tcp)
        let transport = JITIPCChannel(connection: connection, token: token,
                                      message: { [weak self] command in
            self?.worker.async { [weak self] in self?.run(command) }
        }, ended: { [weak self] _ in
            self?.worker.async { [weak self] in
                self?.requestContext?.cancelRequest(withError: NSError(domain: "77U.JIT", code: 11))
            }
        })
        requestContext = context
        channel = transport
        transport.start()
        transport.send(JITIPCMessage(action: "hello"))
    }

    private func run(_ command: JITIPCMessage) {
        switch command.action {
        case "detectTXM":
            channel?.send(JITIPCMessage(action: "txm", txm: StikJIT.isTXMPresent))
        case "resetDDI":
            do {
                try StikJIT.resetCachedDDI(at: diskImages)
                channel?.send(JITIPCMessage(action: "reset", ok: true))
            } catch {
                channel?.send(JITIPCMessage(action: "reset", ok: false, text: error.localizedDescription))
            }
        case "prepare":
            guard let pairing = command.pairing else {
                channel?.send(JITIPCMessage(action: "prepared", text: "Missing pairing file", stage: "preparationFailed"))
                return
            }
            let defaultPort: UInt16 = Self.isIPad ? 52360 : 49152
            let port = (command.rsdPort != nil && command.rsdPort! > 0) ? command.rsdPort! : defaultPort
            prepare(using: pairing, deviceAddress: Self.sanitizeAddress(command.deviceAddress), rsdPort: port)
        case "enableJIT":
            guard let pid = command.pid, pid > 0, let pairing = command.pairing else {
                channel?.send(JITIPCMessage(action: "finished", ok: false, text: "Invalid JIT request"))
                return
            }
            let defaultPort: UInt16 = Self.isIPad ? 52360 : 49152
            let port = (command.rsdPort != nil && command.rsdPort! > 0) ? command.rsdPort! : defaultPort
            enableJIT(pid: pid, pairing: pairing, deviceAddress: Self.sanitizeAddress(command.deviceAddress), rsdPort: port)
        default:
            channel?.send(JITIPCMessage(action: "log", text: "Unknown helper command"))
        }
    }

    private static let defaultAddress = "10.7.0.1"

    private static func sanitizeAddress(_ rawAddress: String?) -> String {
        guard let addr = rawAddress?.trimmingCharacters(in: .whitespacesAndNewlines),
              !addr.isEmpty, !addr.hasPrefix("10.7.1."), addr != "0.0.0.0", addr != "127.0.0.1" else {
            return defaultAddress
        }
        return addr
    }

    private static var isIPad: Bool {
        var sysinfo = utsname()
        if uname(&sysinfo) == 0 {
            let machine = withUnsafeBytes(of: &sysinfo.machine) { raw in
                String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
            }
            if machine.hasPrefix("iPad") { return true }
            if machine.hasPrefix("iPhone") { return false }
        }
        return false
    }

    private func handleTransientRecovery(attempt: Int, maxAttempts: Int,
                                         activeAddress: inout String, activePort: inout UInt16) {
        channel?.send(JITIPCMessage(action: "log", text: "Waiting for device JIT service to recover (\(attempt)/\(maxAttempts))…"))
        let sleepMs = min(2000, 700 + attempt * 200)
        Thread.sleep(forTimeInterval: Double(sleepMs) / 1000.0)
        if activeAddress != Self.defaultAddress {
            channel?.send(JITIPCMessage(action: "log", text: "Reverting destination address to \(Self.defaultAddress)"))
            activeAddress = Self.defaultAddress
        }
        if let discovered = Self.findLocalRSDPort(host: activeAddress) {
            if discovered != activePort {
                channel?.send(JITIPCMessage(action: "log", text: "RSD port shifted to \(discovered); switching"))
                activePort = discovered
            }
        } else {
            let candidatePorts: [UInt16] = Self.isIPad ? [52360, 49152, 49153, 49154, 50000] : [49152, 52360, 49153, 49154, 50000]
            let nextIndex = attempt % candidatePorts.count
            let nextPort = candidatePorts[nextIndex]
            if nextPort != activePort {
                channel?.send(JITIPCMessage(action: "log", text: "Trying candidate RSD port \(nextPort) (\(attempt)/\(maxAttempts))…"))
                activePort = nextPort
            }
        }
    }

    private func prepare(using pairing: Data, deviceAddress: String, rsdPort: UInt16) {
        do {
            let lease = try PairingLease(data: pairing)
            defer { lease.remove() }
            var activeAddress = Self.sanitizeAddress(deviceAddress)
            var activePort = rsdPort
            var lastResult: StikJIT.JITReadiness = .preparationFailed(reason: "Unknown")
            let maxAttempts = 10
            for attempt in 1...maxAttempts {
                let result = StikJIT.prepareDevice(pairingFile: lease.url, paths: diskImages,
                                                   configuration: StikJIT.Configuration(deviceAddress: activeAddress, rsdPort: activePort)) { [weak self] stage in
                    self?.report(stage)
                }
                lastResult = result
                switch result {
                case .ready:
                    break
                case .unreachable(let reason), .preparationFailed(let reason):
                    if attempt < maxAttempts && Self.isTransientSocketError(reason: reason) {
                        handleTransientRecovery(attempt: attempt, maxAttempts: maxAttempts,
                                                activeAddress: &activeAddress, activePort: &activePort)
                        continue
                    }
                }
                break
            }
            switch lastResult {
            case .ready(let security):
                channel?.send(JITIPCMessage(action: "prepared", stage: "ready", txm: security.isTXMPresent, rsdPort: activePort))
            case .unreachable(let reason):
                channel?.send(JITIPCMessage(action: "prepared", text: reason, stage: "unreachable"))
            case .preparationFailed(let reason):
                channel?.send(JITIPCMessage(action: "prepared", text: reason, stage: "preparationFailed"))
            }
        } catch {
            channel?.send(JITIPCMessage(action: "prepared", text: error.localizedDescription, stage: "preparationFailed"))
        }
    }

    private func enableJIT(pid: Int32, pairing: Data, deviceAddress: String, rsdPort: UInt16) {
        do {
            let lease = try PairingLease(data: pairing)
            defer { lease.remove() }
            var activeAddress = Self.sanitizeAddress(deviceAddress)
            var activePort = rsdPort
            var lastError: Error?
            let maxAttempts = 10
            for attempt in 1...maxAttempts {
                do {
                    try StikJIT.enableJIT(targetPID: pid, pairingFile: lease.url,
                                         ddiPaths: diskImages,
                                         configuration: StikJIT.Configuration(deviceAddress: activeAddress, rsdPort: activePort),
                                         script: .universal, forceScript: true,
                                         preparationProgress: { [weak self] stage in self?.report(stage) },
                                         progress: { [weak self] line in
                        self?.channel?.send(JITIPCMessage(action: "log", text: line))
                    })
                    lastError = nil
                    break
                } catch {
                    lastError = error
                    let errorDesc = error.localizedDescription
                    channel?.send(JITIPCMessage(action: "log", text: "JIT attempt \(attempt)/\(maxAttempts) socket issue: \(errorDesc)"))
                    guard attempt < maxAttempts && Self.isTransientSocketError(error) else {
                        throw error
                    }
                    handleTransientRecovery(attempt: attempt, maxAttempts: maxAttempts,
                                            activeAddress: &activeAddress, activePort: &activePort)
                }
            }
            if let err = lastError { throw err }
            channel?.send(JITIPCMessage(action: "finished", ok: true, rsdPort: activePort))
        } catch {
            channel?.send(JITIPCMessage(action: "finished", ok: false, text: error.localizedDescription))
        }
    }

    private static func isTransientSocketError(_ error: Error) -> Bool {
        return isTransientSocketError(reason: error.localizedDescription)
    }

    private static func isTransientSocketError(reason: String) -> Bool {
        let r = reason.lowercased()
        return r.contains("54") ||
               r.contains("connectionreset") ||
               r.contains("reset by peer") ||
               r.contains("61") ||
               r.contains("refused") ||
               r.contains("60") ||
               r.contains("timed out") ||
               r.contains("timeout") ||
               r.contains("32") ||
               r.contains("broken pipe") ||
               r.contains("57") ||
               r.contains("not connected") ||
               r.contains("network is unreachable") ||
               r.contains("idevice code 1/0")
    }

    private static func findLocalRSDPort(host: String) -> UInt16? {
        let safeHost = sanitizeAddress(host)
        let isPad = Self.isIPad
        let priorityPorts: [UInt16] = isPad ? [52360, 49152, 49153, 49154, 50000] : [49152, 52360, 49153, 49154, 50000]
        for port in priorityPorts {
            if isPortOpen(host: safeHost, port: port, timeoutMs: 500) { return port }
        }
        return nil
    }

    private static func isPortOpen(host: String, port: UInt16, timeoutMs: Int = 500) -> Bool {
        let sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)
        guard sock >= 0 else { return false }
        defer { close(sock) }

        let flags = fcntl(sock, F_GETFL, 0)
        _ = fcntl(sock, F_SETFL, flags | O_NONBLOCK)

        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        guard host.withCString({ inet_pton(AF_INET, $0, &addr.sin_addr) }) == 1 else { return false }

        let ret = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(sock, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        if ret == 0 { return true }
        if errno != EINPROGRESS { return false }

        var pfd = pollfd(fd: sock, events: Int16(POLLOUT), revents: 0)
        guard poll(&pfd, 1, Int32(timeoutMs)) > 0 else { return false }
        if (pfd.revents & Int16(POLLERR | POLLHUP | POLLNVAL)) != 0 { return false }
        if (pfd.revents & Int16(POLLOUT)) == 0 { return false }

        var err: Int32 = 0
        var errLen = socklen_t(MemoryLayout<Int32>.size)
        if getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &errLen) != 0 || err != 0 { return false }

        var peer = sockaddr_in()
        var peerLen = socklen_t(MemoryLayout<sockaddr_in>.size)
        return withUnsafeMutablePointer(to: &peer) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                getpeername(sock, $0, &peerLen) == 0
            }
        }
    }

    private func report(_ stage: StikJIT.PreparationStage) {
        let name: String
        let detail: String?
        let fraction: Double
        switch stage {
        case .checkingReachability: (name, detail, fraction) = ("Checking Network", nil, 0)
        case .checkingDDI: (name, detail, fraction) = ("Checking DDI", nil, 0)
        case .downloadingDDI(let progress, let status): (name, detail, fraction) = ("Downloading", status, progress)
        case .mountingDDI(let progress): (name, detail, fraction) = ("Mounting", nil, progress)
        case .verifyingDDI: (name, detail, fraction) = ("Verifying", nil, 1)
        case .ready: (name, detail, fraction) = ("Ready", nil, 1)
        }
        channel?.send(JITIPCMessage(action: "preparationProgress", text: detail,
                                    stage: name, fraction: fraction))
    }
}

private struct PairingLease {
    let url: URL

    init(data: Data) throws {
        guard !data.isEmpty, data.count < 2 * 1024 * 1024 else {
            throw CocoaError(.fileReadCorruptFile)
        }
        url = FileManager.default.temporaryDirectory
            .appendingPathComponent("77u-pairing-\(UUID().uuidString).plist")
        try data.write(to: url, options: [.atomic, .completeFileProtection])
    }

    func remove() { try? FileManager.default.removeItem(at: url) }
}
