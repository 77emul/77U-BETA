// SPDX-License-Identifier: MPL-2.0
import SwiftUI
import UniformTypeIdentifiers
import Network
import Darwin
import dnssd

private struct JITRunnerReference: @unchecked Sendable {
    let runner: JITRunnerClient
}

enum JITMethod: String, CaseIterable, Identifiable {
    case builtIn, stikDebug, manual
    var id: Self { self }
    var title: String {
        switch self {
        case .builtIn: return "Built-in StikJIT"
        case .stikDebug: return "StikDebug app"
        case .manual: return "Wait for debugger"
        }
    }
}

@MainActor
final class JITCoordinator: NSObject, ObservableObject {
    static let shared = JITCoordinator()
    @Published var method: JITMethod {
        didSet { UserDefaults.standard.set(method.rawValue, forKey: "77U.JIT.method") }
    }
    @Published private(set) var status = "Not prepared"
    @Published private(set) var error: String?
    @Published private(set) var busy = false
    @Published private(set) var pairingName: String
    @Published private(set) var txmStatus = "Unknown"
    private var launcher: CemuJITLauncher?
    private var ipcListener: NWListener?
    private var ipcChannel: JITIPCChannel?
    private var ipcID = UUID()
    private var bootstrap: CheckedContinuation<JITRunnerReference, Error>?
    private var runner: JITRunnerClient?
    private var acquisition: Task<Bool, Never>?
    private var debuggerPreparation: Task<Bool, Never>?
    private var automaticPreparation: Task<Void, Never>?
    private var helperResult: Bool?
    private var operationID = UUID()
    private var testingHelper = false
    private var waitingForVPNReturn = false
    private var returnedFromVPN = false
    private let pairingURL: URL
    var inLiveContainer: Bool { getenv("LC_HOME_PATH") != nil }
    var availableMethods: [JITMethod] {
        inLiveContainer ? [.manual, .stikDebug] : JITMethod.allCases
    }
    var hasPairing: Bool { FileManager.default.isReadableFile(atPath: pairingURL.path) }
    var ready: Bool { CemuBridge_IsJITMemoryReady() }

    override private init() {
        let defaults = UserDefaults.standard
        let isLC = getenv("LC_HOME_PATH") != nil
        let savedRaw = defaults.string(forKey: "77U.JIT.method")
        if isLC {
            if savedRaw == nil || savedRaw == JITMethod.builtIn.rawValue {
                method = .manual
            } else {
                method = JITMethod(rawValue: savedRaw ?? "") ?? .manual
            }
        } else {
            method = JITMethod(rawValue: savedRaw ?? "") ?? .builtIn
        }
        pairingName = defaults.string(forKey: "77U.JIT.pairingName") ?? "Not imported"
        pairingURL = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("77U/StikJIT/pairingFile.plist")
        super.init()
    }

    func importPairing(_ url: URL) {
        guard !busy else { return }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        do {
            let data = try Data(contentsOf: url)
            guard data.count > 0, data.count < 2 * 1024 * 1024,
                  let plist = try PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
                  !plist.isEmpty else {
                throw CocoaError(.fileReadCorruptFile)
            }
            let directory = pairingURL.deletingLastPathComponent()
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            var excludedDirectory = directory
            var resourceValues = URLResourceValues()
            resourceValues.isExcludedFromBackup = true
            try excludedDirectory.setResourceValues(resourceValues)
            try data.write(to: pairingURL, options: [.atomic, .completeFileProtection])
            pairingName = url.lastPathComponent
            UserDefaults.standard.set(pairingName, forKey: "77U.JIT.pairingName")
            error = nil
            status = "Pairing file imported"
            txmStatus = "Unknown"
        } catch { self.error = "Could not import the pairing file: \(error.localizedDescription)" }
    }

    // iLoader places files through House Arrest in Documents. Move a validated
    // copy into private, backup-excluded storage; never export pairing credentials.
    func importPlacedPairingIfNeeded() {
        guard !busy else { return }
        let incoming = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("StikJIT/pairingFile.plist")
        guard FileManager.default.isReadableFile(atPath: incoming.path) else { return }
        importPairing(incoming)
        guard error == nil else { return }
        do {
            try FileManager.default.removeItem(at: incoming)
            pairingName = "Imported from iLoader / Files"
            UserDefaults.standard.set(pairingName, forKey: "77U.JIT.pairingName")
        } catch {
            self.error = "Pairing imported, but the incoming copy could not be removed from Files. Remove StikJIT/pairingFile.plist after setup."
        }
    }

    func removePairing() {
        guard !busy else { return }
        do {
            if hasPairing { try FileManager.default.removeItem(at: pairingURL) }
            pairingName = "Not imported"
            UserDefaults.standard.removeObject(forKey: "77U.JIT.pairingName")
            status = "Not prepared"
            error = nil
        } catch { self.error = error.localizedDescription }
    }

    private func preflight() -> Bool {
        error = nil
        importPlacedPairingIfNeeded()
        guard #available(iOS 17.4, *) else { error = "Built-in JIT requires iOS 17.4 or later."; return false }
        guard CemuJITHasGetTaskAllow() else {
            error = "This installation cannot enable JIT. Re-sign the IPA with get-task-allow enabled."
            return false
        }
        if inLiveContainer && method == .builtIn {
            error = "Built-in StikJIT cannot run inside LiveContainer. Use LiveContainer JIT, StikDebug, or Wait for debugger."
            method = .manual
            return false
        }
        if method == .builtIn {
            guard hasPairing else { error = "Import this device's pairing file first."; return false }
        }
        return true
    }

    func ensureReady() async -> Bool {
        if ready { status = "JIT ready"; return true }
        if let acquisition { return await acquisition.value }
        if let debuggerPreparation { return await debuggerPreparation.value }
        if CemuBridge_HasJITDebugger() {
            busy = true
            status = "Preparing executable memory…"
            let preparation = Task.detached(priority: .userInitiated) {
                CemuBridge_PrepareJITMemory()
            }
            debuggerPreparation = preparation
            let prepared = await preparation.value
            debuggerPreparation = nil
            busy = false
            if prepared && ready {
                error = nil
                status = "JIT ready"
                return true
            }
            error = "JIT is attached, but executable memory preparation failed. Restart 77U and enable JIT again."
            status = "JIT preparation failed"
            return false
        }
        guard preflight() else { return false }
        busy = true
        status = "Preparing JIT…"
        helperResult = nil
        let id = UUID()
        operationID = id
        let task = Task { await self.acquire(id: id) }
        acquisition = task
        let result = await task.value
        acquisition = nil
        busy = false
        if result { status = "JIT ready" }
        else if error == nil { error = "JIT could not be enabled. Restart 77U and try again." }
        stopHelper()
        return result
    }

    private func connectHelper() async throws -> JITRunnerReference {
        let id = UUID()
        ipcID = id
        let token = UUID().uuidString + UUID().uuidString
        let parameters = NWParameters.tcp
        parameters.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: .any)
        let listener = try NWListener(using: parameters)
        ipcListener = listener
        return try await withCheckedThrowingContinuation { continuation in
            bootstrap = continuation
            listener.stateUpdateHandler = { [weak self] state in
                Task { @MainActor in
                    guard let self, self.ipcID == id else { return }
                    switch state {
                    case .ready:
                        guard self.launcher == nil, let port = listener.port else { return }
                        let launcher = CemuJITLauncher()
                        self.launcher = launcher
                        Task { @MainActor [weak self] in
                            do {
                                try await launcher.start(port: port.rawValue, token: token)
                            } catch {
                                self?.helperFailed(error.localizedDescription, id: id)
                            }
                        }
                    case .failed(let error): self.helperFailed(error.localizedDescription, id: id)
                    default: break
                    }
                }
            }
            listener.newConnectionHandler = { [weak self] connection in
                Task { @MainActor in
                    guard let self, self.ipcID == id, self.ipcChannel == nil else { connection.cancel(); return }
                    let channel = JITIPCChannel(connection: connection, token: token, message: { [weak self] message in
                        Task { @MainActor in
                            guard let self, self.ipcID == id else { return }
                            self.receiveHelper(message)
                        }
                    }, ended: { [weak self] reason in
                        Task { @MainActor in self?.helperFailed(reason, id: id) }
                    })
                    self.ipcChannel = channel
                    channel.start()
                }
            }
            listener.start(queue: DispatchQueue(label: "77U.JIT.helper-listener"))
            Task { @MainActor [weak self] in
                try? await Task.sleep(for: .seconds(12))
                guard let self, self.ipcID == id, self.bootstrap != nil else { return }
                self.helperFailed("Timed out starting the JIT helper.", id: id)
            }
        }
    }

    private func receiveHelper(_ value: JITIPCMessage) {
        switch value.action {
        case "hello":
            guard let bootstrap, let ipcChannel else { return }
            self.bootstrap = nil
            bootstrap.resume(returning: JITRunnerReference(runner: JITRunnerClient(channel: ipcChannel)))
        case "txm": txmDetectionFinished(value.txm.map(NSNumber.init(value:)))
        case "preparationProgress": preparationProgress(value.stage ?? "Preparing", fraction: value.fraction ?? 0, detail: value.text)
        case "prepared":
            if let port = value.rsdPort { UserDefaults.standard.set(Int(port), forKey: "77U.JIT.lastWorkingRSDPort") }
            preparationFinished(value.stage ?? "failed", reason: value.text, txmPresent: value.txm.map(NSNumber.init(value:)))
        case "reset": ddiResetFinished(value.ok == true, error: value.text)
        case "log": if busy { status = value.text ?? "Preparing JIT…" }
        case "finished":
            if value.ok == true, let port = value.rsdPort { UserDefaults.standard.set(Int(port), forKey: "77U.JIT.lastWorkingRSDPort") }
            jitFinished(value.ok == true, error: value.text)
        default: helperFailed("Unknown JIT helper response.", id: ipcID)
        }
    }

    private func helperFailed(_ reason: String, id: UUID) {
        guard ipcID == id else { return }
        let formatted = Self.formatJITErrorMessage(reason)
        error = formatted
        helperResult = false
        if let bootstrap {
            self.bootstrap = nil
            bootstrap.resume(throwing: NSError(domain: "77U.JIT", code: 1, userInfo: [NSLocalizedDescriptionKey: formatted]))
        }
        if acquisition == nil { finishUtility() }
    }

    private func stopHelper() {
        ipcID = UUID() // Ignore callbacks already queued by a previous connection.
        ipcListener?.cancel()
        ipcListener = nil
        ipcChannel?.close()
        ipcChannel = nil
        launcher?.invalidate()
        launcher = nil
        runner = nil
    }

    private func acquire(id: UUID) async -> Bool {
        do {
            switch method {
            case .builtIn:
                guard await ensureLocalTunnel() else { return false }
                let reference = try await connectHelper()
                guard operationID == id else { return false }
                runner = reference.runner
                let data = try Data(contentsOf: pairingURL)
                let rsdPort = await Self.remotePairingPort(for: data)
                runner?.enableJIT(forParentPID: getpid(), pairingData: data,
                                  deviceAddress: Self.localGateway,
                                  rsdPort: rsdPort, forceScript: true)
            case .stikDebug:
                var parts = URLComponents()
                parts.scheme = "stikdebug"
                parts.host = "enable-jit"
                // 77U's supported iOS 26 allocator requires the universal protocol.
                parts.queryItems = [URLQueryItem(name: "bundle-id", value: Bundle.main.bundleIdentifier),
                                    URLQueryItem(name: "pid", value: String(getpid())),
                                    URLQueryItem(name: "script-name", value: "universal.js")]
                guard let url = parts.url, await UIApplication.shared.open(url) else {
                    error = "StikDebug could not be opened. Install StikDebug or select another JIT method."
                    return false
                }
                status = "Complete the request in StikDebug, then return to 77U."
            case .manual:
                status = inLiveContainer ? "Warte auf LiveContainer JIT…" : "Connect StikDebug with the Universal script, then return to 77U."
            }
            let waitSeconds: TimeInterval = inLiveContainer ? 5 : (method == .builtIn ? 300 : 120)
            let deadline = Date().addingTimeInterval(waitSeconds)
            while Date() < deadline && operationID == id && !Task.isCancelled {
                if helperResult == false { return false }
                if CemuBridge_HasJITDebugger() {
                    status = "Preparing executable memory…"
                    let prepared = await Task.detached(priority: .userInitiated) {
                        CemuBridge_PrepareJITMemory()
                    }.value
                    guard prepared else { error = "Executable memory preparation failed."; return false }
                    // A built-in helper finishes only after the universal detach command.
                    if method == .builtIn {
                        await Task.detached(priority: .userInitiated) { CemuBridge_DetachJITDebugger() }.value
                        let completionDeadline = Date().addingTimeInterval(15)
                        while helperResult == nil && Date() < completionDeadline {
                            try await Task.sleep(for: .milliseconds(100))
                        }
                        guard helperResult == true else {
                            if error == nil { error = "The JIT helper did not finish. Restart 77U before retrying." }
                            return false
                        }
                    }
                    return ready
                }
                try await Task.sleep(for: .milliseconds(100))
            }
            if inLiveContainer {
                error = "LiveContainer JIT is not active. Choose StikDebug in Settings -> JIT, then enable Launch with JIT for 77U and select JIT/universal.js as its launch script."
            } else {
                error = "JIT setup timed out. Check LocalDevVPN and the pairing file, then restart 77U."
            }
        } catch { self.error = error.localizedDescription }
        return false
    }

    // LocalDevVPN opens this scheme after requesting a connection. This only resumes
    // an existing user-initiated operation; an incoming URL cannot initiate JIT.
    func handleVPNReturn(_ url: URL) {
        guard url.scheme == "cemu77u-jit", waitingForVPNReturn else { return }
        returnedFromVPN = true
    }

    func applicationBecameActive() {
        importPlacedPairingIfNeeded()
        if waitingForVPNReturn { returnedFromVPN = true }
        if ready {
            status = "JIT ready"
            return
        }
        guard inLiveContainer, !busy else { return }
        guard CemuBridge_HasJITDebugger() else {
            status = "LiveContainer JIT is not attached to this 77U process"
            return
        }
        status = "JIT attached; preparing executable memory…"
        if automaticPreparation == nil {
            automaticPreparation = Task {
                _ = await ensureReady()
                automaticPreparation = nil
            }
        }
    }

    private struct LocalTunnelInfo: Sendable {
        let isActive: Bool
        let isTCPReachable: Bool
    }

    nonisolated private static let localGateway = "10.7.0.1"

    private func ensureLocalTunnel() async -> Bool {
        status = "Checking local JIT connection…"
        var tunnel = await Self.localTunnelInfo()
        if tunnel.isTCPReachable { return true }
        if tunnel.isActive {
            // VPN is already active; give the RSD daemon up to 4.2s to settle after rapid restarts
            for _ in 0..<12 {
                try? await Task.sleep(for: .milliseconds(350))
                tunnel = await Self.localTunnelInfo()
                if tunnel.isTCPReachable { return true }
            }
            if tunnel.isActive { return true }
        }
        guard !Task.isCancelled else { return false }
        let url = URL(string: "localdevvpn://enable?scheme=cemu77u-jit")!
        guard UIApplication.shared.canOpenURL(url) else {
            error = "Install LocalDevVPN and open it once to allow its VPN configuration. Then return to 77U and tap Play."
            return false
        }
        var bgTask: UIBackgroundTaskIdentifier = .invalid
        bgTask = UIApplication.shared.beginBackgroundTask(withName: "77U.LocalDevVPN") {
            if bgTask != .invalid {
                UIApplication.shared.endBackgroundTask(bgTask)
                bgTask = .invalid
            }
        }
        waitingForVPNReturn = true
        returnedFromVPN = false
        defer {
            waitingForVPNReturn = false
            returnedFromVPN = false
            if bgTask != .invalid {
                UIApplication.shared.endBackgroundTask(bgTask)
                bgTask = .invalid
            }
        }
        status = "Connecting LocalDevVPN… Allow the VPN setup if asked, then return to 77U."
        CemuBridge_LogJITMilestone("[JIT] Local endpoint unreachable; opening LocalDevVPN")
        guard await UIApplication.shared.open(url) else {
            error = "Could not open LocalDevVPN. Open it, connect, and return to 77U."
            return false
        }
        let returnDeadline = Date().addingTimeInterval(120)
        do {
            while !returnedFromVPN || UIApplication.shared.applicationState != .active {
                guard Date() < returnDeadline, !Task.isCancelled else {
                    error = "Return from LocalDevVPN and tap Play again to continue."
                    return false
                }
                try await Task.sleep(for: .milliseconds(150))
            }
            status = "Waiting for LocalDevVPN to establish connection…"
            let connectionDeadline = Date().addingTimeInterval(25)
            var established = false
            repeat {
                guard !Task.isCancelled else { return false }
                tunnel = await Self.localTunnelInfo()
                if tunnel.isTCPReachable {
                    try await Task.sleep(for: .milliseconds(350))
                    established = true
                    break
                }
                try await Task.sleep(for: .milliseconds(300))
            } while Date() < connectionDeadline

            if !established && tunnel.isActive {
                try await Task.sleep(for: .milliseconds(600))
                tunnel = await Self.localTunnelInfo()
                if tunnel.isTCPReachable { established = true }
            }
            if established { return true }
        } catch { return false }
        error = "LocalDevVPN did not connect. Open it, enable the VPN, then return to 77U."
        return false
    }

    private nonisolated static func localTunnelInfo() async -> LocalTunnelInfo {
        await Task.detached(priority: .userInitiated) {
            var foundActiveUtun = false

            // Inspect interfaces with getifaddrs
            var ifaddr: UnsafeMutablePointer<ifaddrs>?
            if getifaddrs(&ifaddr) == 0, let first = ifaddr {
                defer { freeifaddrs(ifaddr) }
                for ptr in sequence(first: first, next: { $0.pointee.ifa_next }) {
                    let ifa = ptr.pointee
                    let name = String(cString: ifa.ifa_name)
                    let flags = ifa.ifa_flags
                    let isUpAndRunning = (flags & UInt32(IFF_UP)) != 0 && (flags & UInt32(IFF_RUNNING)) != 0

                    if (name.hasPrefix("utun") || name.hasPrefix("ipsec") || name.hasPrefix("ppp")) && isUpAndRunning {
                        if let addr = ifa.ifa_addr, addr.pointee.sa_family == sa_family_t(AF_INET) {
                            if (flags & UInt32(IFF_POINTOPOINT)) != 0, let dst = ifa.ifa_dstaddr {
                                var buffer = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                                var sin = dst.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee }
                                if inet_ntop(AF_INET, &sin.sin_addr, &buffer, socklen_t(INET_ADDRSTRLEN)) != nil {
                                    let addrStr = buffer.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
                                    if addrStr.hasPrefix("10.7.1.") { continue }
                                    if addrStr == "10.7.0.1" || addrStr.hasPrefix("10.7.0.") {
                                        foundActiveUtun = true
                                        break
                                    }
                                }
                            }
                            foundActiveUtun = true
                        }
                    }
                }
            }

            // Quick TCP test against the local gateway (10.7.0.1)
            let isPad = Self.isPadDevice
            let defaultPort: UInt16 = isPad ? 52360 : 49152
            let secondaryPort: UInt16 = isPad ? 49152 : 52360
            let cachedPort = UInt16(UserDefaults.standard.integer(forKey: "77U.JIT.lastWorkingRSDPort"))
            let checkPorts: [UInt16] = [defaultPort, secondaryPort, cachedPort, 49153, 49154, 50000, 62078].filter { $0 > 0 }
            var tcpReachable = false
            for port in checkPorts {
                if isPortOpenLocally(host: localGateway, port: port, timeoutMs: 450) {
                    tcpReachable = true
                    break
                }
            }

            return LocalTunnelInfo(isActive: tcpReachable || foundActiveUtun, isTCPReachable: tcpReachable)
        }.value
    }

    private nonisolated static var isPadDevice: Bool {
        var sysinfo = utsname()
        if uname(&sysinfo) == 0 {
            let machine = withUnsafeBytes(of: &sysinfo.machine) { raw in
                String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
            }
            if machine.hasPrefix("iPad") { return true }
            if machine.hasPrefix("iPhone") { return false }
        }
        return true
    }

    private nonisolated static func isPortOpenLocally(host: String, port: UInt16, timeoutMs: Int = 500) -> Bool {
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

    private nonisolated static func remotePairingPort(for pairingData: Data) async -> UInt16 {
        _ = pairingData
        if let port = knownRemotePairingPort() { return port }
        // remotepairingd's listener port is dynamic (e.g. 55621 after a restart);
        // the device announces it via Bonjour, so look it up there.
        for port in await bonjourRemotePairingPorts(timeout: 3).sorted()
        where isPortOpenLocally(host: localGateway, port: port, timeoutMs: 450) {
            CemuBridge_LogJITMilestone("[JIT PORT] Verified Bonjour RSD port \(port) on \(localGateway)")
            UserDefaults.standard.set(Int(port), forKey: "77U.JIT.lastWorkingRSDPort")
            return port
        }
        let defaultPort: UInt16 = isPadDevice ? 52360 : 49152
        CemuBridge_LogJITMilestone("[JIT PORT] Fallback to default port \(defaultPort)")
        return defaultPort
    }

    private nonisolated static func knownRemotePairingPort() -> UInt16? {
        let isPad = Self.isPadDevice
        let defaultPort: UInt16 = isPad ? 52360 : 49152
        let secondaryPort: UInt16 = isPad ? 49152 : 52360

        // Try the port that worked last session, but validate it strictly
        let cached = UInt16(UserDefaults.standard.integer(forKey: "77U.JIT.lastWorkingRSDPort"))
        if cached > 0 {
            if isPortOpenLocally(host: localGateway, port: cached, timeoutMs: 450) {
                print("[JIT PORT] Verified cached RSD port \(cached) on \(localGateway)")
                CemuBridge_LogJITMilestone("[JIT PORT] Verified cached RSD port \(cached) on \(localGateway)")
                return cached
            } else {
                UserDefaults.standard.removeObject(forKey: "77U.JIT.lastWorkingRSDPort")
            }
        }

        // Then the default ports and the dynamic range
        let portsToCheck: [UInt16] = [defaultPort, secondaryPort, 49153, 49154, 50000]
        for port in portsToCheck {
            if isPortOpenLocally(host: localGateway, port: port, timeoutMs: 450) {
                print("[JIT PORT] Verified open RSD port \(port) on \(localGateway)")
                CemuBridge_LogJITMilestone("[JIT PORT] Verified open RSD port \(port) on \(localGateway)")
                UserDefaults.standard.set(Int(port), forKey: "77U.JIT.lastWorkingRSDPort")
                return port
            }
        }
        return nil
    }

    /// State of one Bonjour lookup; only touched on `queue`.
    private final class RemotePairingLookup: @unchecked Sendable {
        let queue = DispatchQueue(label: "77U.JIT.bonjour")
        var ports = Set<UInt16>()
        var resolving: [String: DNSServiceRef] = [:]
    }

    private nonisolated static func bonjourRemotePairingPorts(timeout: TimeInterval) async -> [UInt16] {
        await withCheckedContinuation { (continuation: CheckedContinuation<[UInt16], Never>) in
            let lookup = RemotePairingLookup()
            let browser = NWBrowser(for: .bonjour(type: "_remotepairing._tcp", domain: "local."), using: NWParameters())
            browser.browseResultsChangedHandler = { results, _ in
                for result in results {
                    guard case let .service(name, type, domain, _) = result.endpoint,
                          lookup.resolving[name] == nil else { continue }
                    var ref: DNSServiceRef?
                    let context = Unmanaged.passUnretained(lookup).toOpaque()
                    let error = DNSServiceResolve(&ref, 0, 0, name, type, domain, { _, _, _, error, _, _, port, _, _, context in
                        guard error == kDNSServiceErr_NoError, let context else { return }
                        Unmanaged<RemotePairingLookup>.fromOpaque(context).takeUnretainedValue()
                            .ports.insert(UInt16(bigEndian: port))
                    }, context)
                    if error == kDNSServiceErr_NoError, let ref {
                        DNSServiceSetDispatchQueue(ref, lookup.queue)
                        lookup.resolving[name] = ref
                    }
                }
            }
            browser.start(queue: lookup.queue)
            lookup.queue.asyncAfter(deadline: .now() + timeout) {
                browser.cancel()
                lookup.resolving.values.forEach { DNSServiceRefDeallocate($0) }
                lookup.resolving.removeAll()
                continuation.resume(returning: Array(lookup.ports))
            }
        }
    }

    func runHelperSelfTest() async {
        guard !busy else { return }
        busy = true
        testingHelper = true
        print("[JIT HELPER TEST] Starting helper; no debugger attachment")
        CemuBridge_LogJITMilestone("[JIT HELPER TEST] Starting helper; no debugger attachment")
        do {
            let reference = try await connectHelper()
            runner = reference.runner
            runner?.detectTXM()
        } catch {
            print("[JIT HELPER TEST] Failed: \(error.localizedDescription)")
            CemuBridge_LogJITMilestone("[JIT HELPER TEST] Failed: \(error.localizedDescription)")
            testingHelper = false
            finishUtility()
        }
    }

    func runActivationSelfTest() async {
        let resultURL = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Logs/jit-activation-result.txt")
        try? FileManager.default.removeItem(at: resultURL)
        let succeeded = await ensureReady()
        let message = succeeded ? "PASS: JIT ready" : "FAIL: \(error ?? status)"
        try? message.write(to: resultURL, atomically: true, encoding: .utf8)
    }

    func prepareDevice() async {
        guard !busy, preflight(), method == .builtIn else { return }
        busy = true
        status = "Checking device…"
        guard await ensureLocalTunnel() else { finishUtility(); return }
        do {
            let reference = try await connectHelper()
            runner = reference.runner
            let data = try Data(contentsOf: pairingURL)
            let rsdPort = await Self.remotePairingPort(for: data)
            runner?.prepareDevice(withPairingData: data, deviceAddress: Self.localGateway, rsdPort: rsdPort)
        } catch { self.error = error.localizedDescription; finishUtility() }
    }

    func resetDDI() async {
        guard !busy, preflight(), method == .builtIn else { return }
        busy = true
        do {
            let reference = try await connectHelper()
            runner = reference.runner
            runner?.resetCachedDDI()
        } catch { self.error = error.localizedDescription; finishUtility() }
    }

    private func finishUtility() {
        busy = false
        stopHelper()
    }

    nonisolated func txmDetectionFinished(_ txmPresent: NSNumber?) {
        let value = txmPresent?.boolValue
        Task { @MainActor in
            self.txmStatus = value.map { $0 ? "Present" : "Not present" } ?? "Unknown"
            if self.testingHelper {
                print("[JIT HELPER TEST] Passed; helper IPC received TXM=\(self.txmStatus)")
                CemuBridge_LogJITMilestone("[JIT HELPER TEST] Passed; helper IPC received TXM=\(self.txmStatus)")
                self.testingHelper = false
                self.finishUtility()
            }
        }
    }
    nonisolated func preparationProgress(_ stage: String, fraction: Double, detail: String?) {
        Task { @MainActor in self.status = detail ?? stage }
    }
    nonisolated func preparationFinished(_ readiness: String, reason: String?, txmPresent: NSNumber?) {
        let value = txmPresent?.boolValue
        Task { @MainActor in
            self.status = readiness == "ready" ? "Device prepared" : "Device not ready"
            self.error = reason.map { Self.formatJITErrorMessage($0) }
            self.txmStatus = value.map { $0 ? "Present" : "Not present" } ?? "Unknown"
            self.finishUtility()
        }
    }
    nonisolated func ddiResetFinished(_ ok: Bool, error: String?) {
        Task { @MainActor in
            self.status = ok ? "Developer disk image cache removed" : "Reset failed"
            self.error = error
            self.finishUtility()
        }
    }
    nonisolated func jitLog(_ line: String) {
        // Never persist arbitrary upstream logs: keep pairing details out of exports.
        Task { @MainActor in if self.busy { self.status = line } }
    }
    nonisolated func jitFinished(_ ok: Bool, error: String?) {
        Task { @MainActor in
            self.helperResult = ok
            if !ok { self.helperFailed(error ?? "JIT helper failed.", id: self.ipcID) }
        }
    }

    nonisolated static func formatJITErrorMessage(_ rawReason: String) -> String {
        let isRefused = rawReason.contains("61") || rawReason.contains("refused") || rawReason.contains("16/0")
        if isRefused {
            return """
            Could not connect to LocalDevVPN. The connection was refused.

            If you use cellular data without Wi-Fi:
            1. Turn on Airplane Mode briefly and leave LocalDevVPN active.
            2. Tap Try again.
            3. Turn off Airplane Mode once the game starts.

            Error details: \(rawReason)
            """
        }
        return rawReason
    }
}
