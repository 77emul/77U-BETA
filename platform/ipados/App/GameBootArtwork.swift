import Foundation
import UIKit
import CoreImage

/// Keeps only three decoded backgrounds; archive intermediates never become part of the IPA.
enum GameBootArtwork {
    // NSCache synchronizes its own access; decoded UIImage values are never mutated.
    private final class ImageStorage: @unchecked Sendable {
        let images = NSCache<NSString, UIImage>()
        let context = CIContext(options: [.cacheIntermediates: false])
        init() { images.countLimit = 3; images.totalCostLimit = 24 * 1024 * 1024 }
    }
    private static let storage = ImageStorage()
    private static let extractionLock = NSLock()

    static func load(titleURL: URL, isGameFile: Bool) -> UIImage? {
        let accessing = titleURL.startAccessingSecurityScopedResource()
        defer { if accessing { titleURL.stopAccessingSecurityScopedResource() } }
        let source = isGameFile ? titleURL : titleURL.appendingPathComponent("meta/bootTvTex.tga")
        guard let attributes = try? FileManager.default.attributesOfItem(atPath: source.path) else { return nil }
        let key = "\(source.path)|\(attributes[.size] ?? 0)|\(attributes[.modificationDate] ?? Date.distantPast)" as NSString
        if let cached = storage.images.object(forKey: key) { return cached }
        let image: UIImage? = extractionLock.withLock {
            guard let data = loadData(titleURL: titleURL, isGameFile: isGameFile, sound: false) else { return nil }
            guard let decoded = GameLibraryIconReader.decodeUncompressedTGA(data, maximumDimension: 2048),
                  let cgImage = decoded.cgImage else { return nil }
            let source = CIImage(cgImage: cgImage)
            // Soften the scenery behind the carousel once, before putting it in the cache.
            let backdrop = source.clampedToExtent()
                .applyingFilter("CIGaussianBlur", parameters: [kCIInputRadiusKey: source.extent.width * 0.005])
                .cropped(to: source.extent)
                .applyingFilter("CIColorControls", parameters: [kCIInputSaturationKey: 0.86])
            guard let softened = storage.context.createCGImage(backdrop, from: source.extent) else { return decoded }
            return UIImage(cgImage: softened)
        }
        if let image, let cg = image.cgImage { storage.images.setObject(image, forKey: key, cost: cg.width * cg.height * 4) }
        return image
    }

    static func loadSound(titleURL: URL, isGameFile: Bool) -> Data? {
        let accessing = titleURL.startAccessingSecurityScopedResource()
        defer { if accessing { titleURL.stopAccessingSecurityScopedResource() } }
        return extractionLock.withLock {
            guard let data = loadData(titleURL: titleURL, isGameFile: isGameFile, sound: true) else { return nil }
            return WiiUBootSound.waveData(from: data, preview: false)
        }
    }

    private static func loadData(titleURL: URL, isGameFile: Bool, sound: Bool) -> Data? {
        let name = sound ? "bootSound.btsnd" : "bootTvTex.tga"
        let file = isGameFile
            ? FileManager.default.temporaryDirectory.appendingPathComponent("77U-boot-\(UUID().uuidString)-\(name)")
            : titleURL.appendingPathComponent("meta/\(name)")
        defer { if isGameFile { try? FileManager.default.removeItem(at: file) } }
        if isGameFile {
            let extracted = titleURL.path.withCString { path in
                file.path.withCString { CemuBridge_ExtractGameBootMedia(path, $0, sound) }
            }
            guard extracted else { return nil }
        }
        guard let size = try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize,
              size >= (sound ? 12 : 18), size <= (sound ? 8 : 16) * 1024 * 1024 else { return nil }
        return try? Data(contentsOf: file, options: .mappedIfSafe)
    }
}
