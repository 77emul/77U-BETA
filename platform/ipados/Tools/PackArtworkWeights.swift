import Foundation
import CryptoKit

struct WeightManifest: Codable {
    let version: Int
    let shuffleWidth: Int
    let originalBytes: Int
    let packedSHA256: String
    let weightSHA256: String
    let modelSHA256: String
    let files: [String: String]
}
func hash(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }
let root = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
let output = URL(fileURLWithPath: CommandLine.arguments[2], isDirectory: true)
let manager = FileManager.default
for model in try manager.contentsOfDirectory(at: root, includingPropertiesForKeys: nil).filter({ $0.pathExtension == "mlmodelc" }) {
    let files = manager.enumerator(at: model, includingPropertiesForKeys: [.isRegularFileKey])!.compactMap { $0 as? URL }.filter { (try? $0.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true }.filter { !$0.pathComponents.contains(where: { $0 == ".DS_Store" || $0 == "__MACOSX" || $0.hasPrefix("._") }) }
    var hashes: [String: String] = [:]
    for file in files { hashes[String(file.path.dropFirst(model.path.count + 1))] = hash(try Data(contentsOf: file)) }
    let modelHash = hash(Data(hashes.keys.sorted().map { "\($0):\(hashes[$0]!)" }.joined(separator: "\n").utf8))
    let destination = output.appendingPathComponent(model.lastPathComponent).appendingPathComponent("weights")
    let manifestURL = destination.appendingPathComponent("weight.manifest.json")
    let packedURL = destination.appendingPathComponent("weight.bin.packed")
    if let existing = try? JSONDecoder().decode(WeightManifest.self, from: Data(contentsOf: manifestURL)), existing.modelSHA256 == modelHash, let packed = try? Data(contentsOf: packedURL), hash(packed) == existing.packedSHA256 { continue }
    let weights = try Data(contentsOf: model.appendingPathComponent("weights/weight.bin"))
    // Byte-plane shuffling improves lossless compression; it never changes a model value.
    let original = [UInt8](weights)
    var shuffled = [UInt8](); shuffled.reserveCapacity(original.count)
    for lane in 0..<2 { for index in stride(from: lane, to: original.count, by: 2) { shuffled.append(original[index]) } }
    let packed = try (Data(shuffled) as NSData).compressed(using: .lzma) as Data
    let roundTrip = try (packed as NSData).decompressed(using: .lzma) as Data
    precondition(roundTrip == Data(shuffled), "Lossless packing verification failed")
    let manifest = WeightManifest(version: 1, shuffleWidth: 2, originalBytes: original.count, packedSHA256: hash(packed), weightSHA256: hash(weights), modelSHA256: modelHash, files: hashes)
    try manager.createDirectory(at: destination, withIntermediateDirectories: true)
    try packed.write(to: packedURL, options: .atomic)
    let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
    try encoder.encode(manifest).write(to: manifestURL, options: .atomic)
    print("Packed \(model.lastPathComponent): \(weights.count) -> \(packed.count) bytes; lossless verification passed")
}
