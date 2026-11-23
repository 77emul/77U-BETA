import Foundation
import CryptoKit

@main struct ArtworkModelArchiveTests {
    static func hash(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }
    static func main() throws {
        let source = URL(fileURLWithPath: CommandLine.arguments[1])
        let packed = URL(fileURLWithPath: CommandLine.arguments[2])
        let scratch = URL(fileURLWithPath: CommandLine.arguments[3])
        let manager = FileManager.default
        try manager.createDirectory(at: scratch, withIntermediateDirectories: true)
        for name in ["Artwork-quality", "Artwork-normal"] {
            let original = source.appendingPathComponent(name + ".mlmodelc")
            let fixture = scratch.appendingPathComponent(name + ".mlmodelc")
            if manager.fileExists(atPath: fixture.path) { try manager.removeItem(at: fixture) }
            try manager.copyItem(at: original, to: fixture)
            try manager.removeItem(at: fixture.appendingPathComponent("weights/weight.bin"))
            for filename in ["weight.bin.packed", "weight.manifest.json"] {
                try manager.copyItem(at: packed.appendingPathComponent(name + ".mlmodelc/weights/" + filename), to: fixture.appendingPathComponent("weights/" + filename))
            }
            let cache = scratch.appendingPathComponent("cache")
            guard let restored = ArtworkModelArchive.prepare(fixture, cacheRoot: cache) else { fatalError("Restore failed: \(name)") }
            for file in manager.enumerator(at: original, includingPropertiesForKeys: [.isRegularFileKey])!.compactMap({ $0 as? URL }) where (try? file.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true {
                let relative = String(file.path.dropFirst(original.path.count + 1))
                let expected = try Data(contentsOf: file)
                let actual = try Data(contentsOf: restored.appendingPathComponent(relative))
                assert(expected == actual, "Changed model bytes: \(name)/\(relative)")
            }
            assert(ArtworkModelArchive.prepare(fixture, cacheRoot: cache) == restored)
            try Data("corrupt cached weight".utf8).write(to: restored.appendingPathComponent("weights/weight.bin"))
            assert(ArtworkModelArchive.prepare(fixture, cacheRoot: cache) == restored, "Cache corruption must be repaired")
            let repaired = try Data(contentsOf: restored.appendingPathComponent("weights/weight.bin"))
            let expectedWeight = try Data(contentsOf: original.appendingPathComponent("weights/weight.bin"))
            assert(repaired == expectedWeight)
            let blob = fixture.appendingPathComponent("weights/weight.bin.packed")
            try Data("corrupt archive".utf8).write(to: blob)
            assert(ArtworkModelArchive.prepare(fixture, cacheRoot: scratch.appendingPathComponent("fresh")) == nil, "Corrupt archives must be rejected")
            print("PASS: \(name), all original model files restored byte-for-byte, cache reuse/repair, corruption rejection")
        }
        // Odd-length byte-plane fixtures catch lane-boundary errors independently of model sizes.
        let odd = scratch.appendingPathComponent("Odd.mlmodelc/weights")
        try manager.createDirectory(at: odd, withIntermediateDirectories: true)
        let original = Data([1,2,3,4,5]), shuffled = Data([1,3,5,2,4])
        let blob = try (shuffled as NSData).compressed(using: .lzma) as Data
        let manifest: [String: Any] = ["version": 1, "shuffleWidth": 2, "originalBytes": 5, "packedSHA256": hash(blob), "weightSHA256": hash(original), "modelSHA256": hash(original), "files": ["weights/weight.bin": hash(original)]]
        try blob.write(to: odd.appendingPathComponent("weight.bin.packed"))
        let manifestURL = odd.appendingPathComponent("weight.manifest.json")
        try JSONSerialization.data(withJSONObject: manifest).write(to: manifestURL)
        let restored = ArtworkModelArchive.prepare(odd.deletingLastPathComponent(), cacheRoot: scratch.appendingPathComponent("odd-cache"))!
        let oddBytes = try Data(contentsOf: restored.appendingPathComponent("weights/weight.bin"))
        assert(oddBytes == original)
        var bad = manifest; bad["files"] = ["../escape": hash(original)]
        try JSONSerialization.data(withJSONObject: bad).write(to: manifestURL)
        assert(ArtworkModelArchive.prepare(odd.deletingLastPathComponent(), cacheRoot: scratch.appendingPathComponent("unsafe-cache")) == nil)
        print("PASS: odd byte counts and path traversal rejection")
    }
}
