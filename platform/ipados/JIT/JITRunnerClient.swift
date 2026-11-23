// SPDX-License-Identifier: MPL-2.0
import Foundation

final class JITRunnerClient: NSObject, @unchecked Sendable {
    let channel: JITIPCChannel
    init(channel: JITIPCChannel) { self.channel = channel }
    func detectTXM() { channel.send(JITIPCMessage(action: "detectTXM")) }
    func prepareDevice(withPairingData data: Data, deviceAddress: String = "10.7.0.1", rsdPort: UInt16) {
        channel.send(JITIPCMessage(action: "prepare", pairing: data, rsdPort: rsdPort, deviceAddress: deviceAddress))
    }
    func resetCachedDDI() { channel.send(JITIPCMessage(action: "resetDDI")) }
    func enableJIT(forParentPID pid: Int32, pairingData: Data, deviceAddress: String = "10.7.0.1", rsdPort: UInt16, forceScript: Bool) {
        channel.send(JITIPCMessage(action: "enableJIT", pid: pid, pairing: pairingData,
                                   rsdPort: rsdPort, deviceAddress: deviceAddress))
    }
}
