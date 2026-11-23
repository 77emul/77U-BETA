import Foundation
import AVFoundation
import UIKit

/// Wii U bootSound.btsnd stores an eight-byte header followed by 48 kHz,
/// stereo, signed 16-bit big-endian PCM.
enum WiiUBootSound {
    static func waveData(from source: Data, preview: Bool = true) -> Data? {
        guard source.count > 8, (source.count - 8) % 4 == 0,
              source.count <= 32 * 1024 * 1024 else { return nil }
        let byteCount = preview ? min(source.count - 8, 48_000 * 4 * 5) : source.count - 8
        var wave = Data(capacity: 44 + byteCount)
        func text(_ value: String) { wave.append(contentsOf: value.utf8) }
        func little<T: FixedWidthInteger>(_ value: T) {
            var encoded = value.littleEndian
            withUnsafeBytes(of: &encoded) { wave.append(contentsOf: $0) }
        }
        text("RIFF"); little(UInt32(36 + byteCount)); text("WAVEfmt ")
        little(UInt32(16)); little(UInt16(1)); little(UInt16(2))
        little(UInt32(48_000)); little(UInt32(192_000)); little(UInt16(4)); little(UInt16(16))
        text("data"); little(UInt32(byteCount))
        let samples = Array(source.dropFirst(8).prefix(byteCount))
        let fadeFrames = preview ? min(4_800, byteCount / 4) : 0
        for offset in stride(from: 0, to: byteCount, by: 2) {
            let bits = UInt16(samples[offset]) << 8 | UInt16(samples[offset + 1])
            var sample = Int32(Int16(bitPattern: bits))
            let framesLeft = (byteCount - offset - 2) / 4
            if framesLeft < fadeFrames { sample = sample * Int32(framesLeft) / Int32(fadeFrames) }
            little(Int16(clamping: sample))
        }
        return wave
    }
}

@MainActor
final class AppStartupSound {
    static let shared = AppStartupSound()
    private var hasEntered = false
    private var stopped = false
    private var player: AVAudioPlayer?

    func playOnEntry() async {
        guard !MenuThemeStore.shared.usesThemeMusic else { return }
        guard !hasEntered else { return }
        hasEntered = true
        guard CemuBridge_GetSetting(CemuIOSSettingPlayBootSound) != 0,
              !GameOrientationCoordinator.isGameActive else { return }
        let wave = await Task.detached(priority: .utility) {
            let path = CemuBridge_GetStartupSoundPath()
            guard path.pointee != 0,
                  let data = try? Data(contentsOf: URL(fileURLWithPath: String(cString: path)), options: .mappedIfSafe)
            else { return Data?.none }
            return WiiUBootSound.waveData(from: data)
        }.value
        guard UIApplication.shared.applicationState == .active,
              !stopped, !GameOrientationCoordinator.isGameActive,
              CemuBridge_GetSetting(CemuIOSSettingPlayBootSound) != 0, let wave else { return }
        do {
            let intro = try AVAudioPlayer(data: wave)
            intro.volume = Float(CemuBridge_GetSetting(CemuIOSSettingTVVolume)) / 100
            player = intro
            intro.play()
        } catch {
            CemuBridge_LogJITMilestone("[AUDIO] Startup sound could not play: \(error.localizedDescription)")
        }
    }

    func stop() {
        stopped = true
        player?.stop()
        player = nil
    }
}
