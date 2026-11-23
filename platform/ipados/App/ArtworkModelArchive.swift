import Foundation
import CryptoKit

/// Materializes the original compiled model offline; weights and graph files are checksum-verified.
enum ArtworkModelArchive {
    private struct Manifest: Decodable {
        let version: Int
        let shuffleWidth: Int
        let originalBytes: Int
        let packedSHA256: String
        let weightSHA256: String
        let modelSHA256: String
        let files: [String: String]
    }
    private static let lock = NSLock()
    private static func hash(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }

    private static let importFiles: [String: [(path: String, bytes: Int, digest: String)]] = [
        "Artwork-normal": [
            ("analytics/coremldata.bin", 243, "68304c11e0bbd51557691f90e93459c9b769bb1e534be86c5cb14e4b236dbe60"),
            ("coremldata.bin", 519, "a6efec81062b25639c4e30744f27ed64af84fd7284a9aa24ccc16378afab694e"),
            ("metadata.json", 2141, "7b02626d2b1af47c6cf4bcbba6cd6dcdbd35daf9c59e2d60baba3292deaf804d"),
            ("model.mil", 73923, "8694e2f2371de9072f369e7eede100d6d52c81765d6fc87afa4240d13412f6b4"),
            ("weights/weight.bin", 1249056, "67a01674b4a2c2835439599627304585b0d73072d07742ab3dad20aee88ca991")
        ],
        "Artwork-quality": [
            ("analytics/coremldata.bin", 243, "a35be543372969f28cb7234d3b1ad5527a500870ff6ff411e07724fcad346872"),
            ("coremldata.bin", 503, "5465e15bfc8262304676617787c799363bc2dfa64c54c72d358a2d18a0454ce6"),
            ("metadata.json", 2151, "b9ad80d0a31e08f6bd3f474ff6f6cac7e9de62baaf69d58de75de2686491de31"),
            ("model.mil", 877699, "ad354479cc403b0d4be6438706f6376325ce5883cb3373b113d150227db11a5b"),
            ("weights/weight.bin", 16960960, "92382fdd8462cb316fe3ed7b18733b428941435b87d4723413faa241873459a3")
        ]
    ]

    private static var importRoot: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("CoverModels", isDirectory: true)
    }

    static func installedModel(named name: String) -> URL? {
        let url = importRoot.appendingPathComponent(name + ".mlmodelc", isDirectory: true)
        return FileManager.default.fileExists(atPath: url.path) ? url : nil
    }

    static func removeInstalledModels(in root: URL? = nil) throws {
        try lock.withLock {
            let directory = root ?? importRoot
            let manager = FileManager.default
            if manager.fileExists(atPath: directory.path) { try manager.removeItem(at: directory) }
        }
    }

    static func isHTTPSDownload(_ url: URL) -> Bool {
        url.scheme?.lowercased() == "https" && url.host != nil && url.user == nil && url.password == nil
    }

    static func download(from url: URL) async throws {
        guard isHTTPSDownload(url), let files = importFiles["Artwork-normal"] else {
            throw CocoaError(.fileReadUnsupportedScheme)
        }
        let limit = files.reduce(0) { $0 + $1.bytes }
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 20
        configuration.timeoutIntervalForResource = 60
        let session = URLSession(configuration: configuration)
        defer { session.invalidateAndCancel() }
        let (stream, response) = try await session.bytes(from: url)
        guard let response = response as? HTTPURLResponse, response.statusCode == 200,
              let finalURL = response.url, isHTTPSDownload(finalURL),
              response.expectedContentLength == -1 || response.expectedContentLength == Int64(limit) else {
            throw CocoaError(.fileReadCorruptFile)
        }
        var contents = Data()
        contents.reserveCapacity(limit)
        for try await byte in stream {
            guard contents.count < limit else { throw CocoaError(.fileReadTooLarge) }
            contents.append(byte)
        }
        try installDownloadedModel(contents)
    }

    static func installDownloadedModel(_ contents: Data, in root: URL? = nil) throws {
        guard let files = importFiles["Artwork-normal"], contents.count == files.reduce(0, { $0 + $1.bytes }) else {
            throw CocoaError(.fileReadCorruptFile)
        }
        let manager = FileManager.default
        let temporary = manager.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        defer { try? manager.removeItem(at: temporary) }
        let model = temporary.appendingPathComponent("Artwork-normal.mlmodelc", isDirectory: true)
        var offset = contents.startIndex
        for file in files {
            let target = model.appendingPathComponent(file.path)
            try manager.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
            try contents[offset..<(offset + file.bytes)].write(to: target)
            offset += file.bytes
        }
        try install(model, in: root)
    }

    static func install(_ selection: URL, in root: URL? = nil) throws {
        let accessing = selection.startAccessingSecurityScopedResource()
        defer { if accessing { selection.stopAccessingSecurityScopedResource() } }
        let source = selection.resolvingSymlinksInPath()
        let name = selection.deletingPathExtension().lastPathComponent
        guard selection.pathExtension == "mlmodelc", let files = importFiles[name] else {
            throw CocoaError(.fileReadUnsupportedScheme)
        }
        try lock.withLock {
            let manager = FileManager.default
            let root = root ?? importRoot
            try manager.createDirectory(at: root, withIntermediateDirectories: true)
            let staging = root.appendingPathComponent(UUID().uuidString, isDirectory: true)
            defer { try? manager.removeItem(at: staging) }
            for file in files {
                let input = source.appendingPathComponent(file.path)
                let resolved = input.resolvingSymlinksInPath()
                let properties = try resolved.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
                guard resolved.path == input.path, properties.isRegularFile == true, properties.fileSize == file.bytes else {
                    throw CocoaError(.fileReadCorruptFile)
                }
                let reader = try FileHandle(forReadingFrom: resolved)
                defer { try? reader.close() }
                let data = try reader.read(upToCount: file.bytes + 1) ?? Data()
                guard data.count == file.bytes, hash(data) == file.digest else { throw CocoaError(.fileReadCorruptFile) }
                let target = staging.appendingPathComponent(file.path)
                try manager.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
                try data.write(to: target)
            }
            let destination = root.appendingPathComponent(name + ".mlmodelc", isDirectory: true)
            if manager.fileExists(atPath: destination.path) {
                _ = try manager.replaceItemAt(destination, withItemAt: staging)
            } else {
                try manager.moveItem(at: staging, to: destination)
            }
        }
    }

    static func prepare(_ model: URL, cacheRoot: URL? = nil) -> URL? {
        let packed = model.appendingPathComponent("weights/weight.bin.packed")
        guard FileManager.default.fileExists(atPath: packed.path) else { return model }
        return lock.withLock {
            do {
                let manager = FileManager.default
                let manifest = try JSONDecoder().decode(Manifest.self, from: Data(contentsOf: model.appendingPathComponent("weights/weight.manifest.json")))
                guard manifest.version == 1, manifest.shuffleWidth == 2,
                      manifest.originalBytes > 0, manifest.originalBytes <= 128 * 1024 * 1024,
                      manifest.modelSHA256.count == 64, manifest.modelSHA256.allSatisfy({ $0.isHexDigit }),
                      manifest.files.count <= 64, manifest.files["weights/weight.bin"] == manifest.weightSHA256,
                      manifest.files.keys.allSatisfy({ !$0.hasPrefix("/") && !$0.split(separator: "/").contains("..") }) else { return nil }
                let root = cacheRoot ?? manager.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("77UArtworkModels", isDirectory: true)
                try manager.createDirectory(at: root, withIntermediateDirectories: true)
                let destination = root.appendingPathComponent("\(model.deletingPathExtension().lastPathComponent)-\(manifest.modelSHA256).mlmodelc", isDirectory: true)
                func valid(_ folder: URL) -> Bool {
                    manifest.files.allSatisfy { path, digest in
                        guard let bytes = try? Data(contentsOf: folder.appendingPathComponent(path), options: .mappedIfSafe) else { return false }
                        return hash(bytes) == digest
                    }
                }
                if manager.fileExists(atPath: destination.path) {
                    if valid(destination) { return destination }
                    try manager.removeItem(at: destination)
                }
                let staging = root.appendingPathComponent("unpacking-\(UUID().uuidString)", isDirectory: true)
                defer { try? manager.removeItem(at: staging) }
                try manager.createDirectory(at: staging, withIntermediateDirectories: true)
                for path in manifest.files.keys where path != "weights/weight.bin" {
                    let target = staging.appendingPathComponent(path)
                    try manager.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
                    try manager.copyItem(at: model.appendingPathComponent(path), to: target)
                }
                let compressed = try Data(contentsOf: packed, options: .mappedIfSafe)
                guard hash(compressed) == manifest.packedSHA256 else { return nil }
                let decoded = try (compressed as NSData).decompressed(using: .lzma) as Data
                guard decoded.count == manifest.originalBytes else { return nil }
                var restored = Data(count: decoded.count)
                decoded.withUnsafeBytes { input in
                    restored.withUnsafeMutableBytes { output in
                        let source = input.bindMemory(to: UInt8.self), target = output.bindMemory(to: UInt8.self)
                        var offset = 0
                        for lane in 0..<manifest.shuffleWidth {
                            for index in stride(from: lane, to: target.count, by: manifest.shuffleWidth) { target[index] = source[offset]; offset += 1 }
                        }
                    }
                }
                guard hash(restored) == manifest.weightSHA256 else { return nil }
                let weightURL = staging.appendingPathComponent("weights/weight.bin")
                try manager.createDirectory(at: weightURL.deletingLastPathComponent(), withIntermediateDirectories: true)
                try restored.write(to: weightURL, options: .atomic)
                guard valid(staging) else { return nil }
                try manager.moveItem(at: staging, to: destination)
                let prefix = model.deletingPathExtension().lastPathComponent + "-"
                for old in (try? manager.contentsOfDirectory(at: root, includingPropertiesForKeys: nil)) ?? [] {
                    if old != destination, old.lastPathComponent.hasPrefix(prefix), old.pathExtension == "mlmodelc" { try? manager.removeItem(at: old) }
                }
                return destination
            } catch { return nil }
        }
    }
}
