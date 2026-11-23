import CoreImage
import CoreImage.CIFilterBuiltins
import CoreML
import CryptoKit
import Foundation
import UIKit

struct GameLibraryArtwork {
    let image: UIImage?
    let cacheKey: String?
    let needsEnhancement: Bool
}

enum ArtworkQuality: String, CaseIterable {
    case normalQuality
    case highQuality

    var modelName: String {
        switch self {
        case .normalQuality: return "Artwork-normal"
        case .highQuality: return "Artwork-quality"
        }
    }

    static var current: Self { .highQuality }
}

enum GameArtworkEnhancer {
    static let targetDimension = 1024
    static let preferenceKey = "CemuIOS.EnhanceLibraryArtwork"
    static let preferenceDidChange = Notification.Name("CemuIOS.EnhanceLibraryArtworkDidChange")
    private static let ignoreBundleKey = "CemuIOS.IgnoreBundledCoverModel"

    private static let algorithmVersion = "native-realesrgan-two-models-v6-1024"
    private static let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
    private static let context = CIContext(options: [.cacheIntermediates: false, .workingColorSpace: colorSpace, .outputColorSpace: colorSpace])

    private final class ModelStorage: @unchecked Sendable {
        let lock = NSLock()
        var model: MLModel?
        var attemptedLoad = false
        var quality: ArtworkQuality?
    }

    private static let modelStorage = ModelStorage()

    static var isDeviceSupportedForNeuralEnhancement: Bool {
        // Neural network models (RealESRGAN / 23-block RRDB) require significant RAM and
        // modern Neural Engine architecture. Devices with < 5.5 GB RAM (such as iPhone 11 with 4 GB)
        // must use the fast, lightweight CoreImage fallback to preserve memory for emulation and avoid Core ML crashes.
        let ram = ProcessInfo.processInfo.physicalMemory
        return ram >= 5500 * 1024 * 1024
    }

    static var isEnabled: Bool {
        guard isDeviceSupportedForNeuralEnhancement else { return false }
        if ignoresBundledModel && !hasNeuralModel { return false }
        let defaults = UserDefaults.standard
        guard defaults.object(forKey: preferenceKey) != nil else { return true }
        return defaults.bool(forKey: preferenceKey)
    }

    static var hasNeuralModel: Bool {
        ArtworkQuality.allCases.contains { modelURL(named: $0.modelName) != nil }
    }

    static var ignoresBundledModel: Bool { UserDefaults.standard.bool(forKey: ignoreBundleKey) }

    static var hasBundledModel: Bool {
        ArtworkQuality.allCases.contains { bundledModelURL(named: $0.modelName) != nil }
    }

    static func useBundledModel(_ enabled: Bool) {
        UserDefaults.standard.set(!enabled, forKey: ignoreBundleKey)
    }

    private static func modelURL(named name: String) -> URL? {
        ArtworkModelArchive.installedModel(named: name) ?? (ignoresBundledModel ? nil : bundledModelURL(named: name))
    }

    private static func bundledModelURL(named name: String) -> URL? {
        Bundle.main.url(forResource: name, withExtension: "mlmodelc") ??
            Bundle.main.url(forResource: name + "-model", withExtension: nil)
    }

    static var shouldPauseForSystemPressure: Bool {
        switch ProcessInfo.processInfo.thermalState {
        case .serious, .critical:
            return true
        default:
            return false
        }
    }

    static func loadArtwork(
        from titleURL: URL,
        useEnhancedCache: Bool = isEnabled
    ) -> GameLibraryArtwork {
        let iconURL = titleURL.appendingPathComponent("meta/iconTex.tga")
        return loadArtwork(fromIcon: iconURL, useEnhancedCache: useEnhancedCache)
    }

    static func loadArtwork(
        fromIcon iconURL: URL,
        useEnhancedCache: Bool = isEnabled
    ) -> GameLibraryArtwork {
        guard FileManager.default.fileExists(atPath: iconURL.path) else {
            return GameLibraryArtwork(image: nil, cacheKey: nil, needsEnhancement: false)
        }

        let key = cacheKey(for: iconURL)
        if useEnhancedCache, let key,
           let cached = cachedImage(for: key, legacyKey: legacyCacheKey(for: iconURL)) {
            return GameLibraryArtwork(image: cached, cacheKey: key, needsEnhancement: false)
        }

        let source = GameLibraryIconReader.readIcon(at: iconURL)
        let longestEdge = source?.cgImage.map { max($0.width, $0.height) } ?? 0
        return GameLibraryArtwork(
            image: source,
            cacheKey: source == nil ? nil : key,
            needsEnhancement: useEnhancedCache && key != nil && longestEdge > 0 && longestEdge < targetDimension
        )
    }

    static func enhance(_ source: CGImage, cacheKey: String, quality: ArtworkQuality = .current) -> UIImage? {
        // Duplicate scans or queued jobs should reuse a completed result too.
        if let cached = cachedImage(for: cacheKey) { return cached }
        let baseline = fallbackUpscale(source)
        var output: CGImage?
        var isNeural = false
        if isDeviceSupportedForNeuralEnhancement,
           source.width > 0 && source.height > 0,
           let baseline, let neural = coreMLUpscale(source, quality: quality) {
            output = preserveSourceShading(source: baseline, neural: neural) ?? neural
            isNeural = true
        } else {
            // Unsupported artwork or lower-RAM devices keep clean aspect ratio through the non-neural fallback.
            output = baseline
        }
        guard let output else { return nil }

        let image = UIImage(cgImage: output, scale: 1, orientation: .up)
        // Only persist to cache if neural upscaling succeeded or if aspect ratio is unsupported or neural is unsupported on device.
        if isNeural || source.width != source.height || !isDeviceSupportedForNeuralEnhancement {
            save(image, for: cacheKey)
        }
        return image
    }

    static func releaseModel() {
        modelStorage.lock.lock()
        modelStorage.model = nil
        modelStorage.attemptedLoad = false
        modelStorage.quality = nil
        modelStorage.lock.unlock()
    }

    static func releaseResourcesAfterMemoryWarning() {
        // The library calls this on its serial artwork queue after any active
        // inference. Never clear shared Core Image resources during a prediction.
        releaseModel()
        context.clearCaches()
    }

    private static func coreMLUpscale(_ source: CGImage, quality: ArtworkQuality) -> CGImage? {
        guard #available(iOS 18.0, *), isDeviceSupportedForNeuralEnhancement else {
            return nil
        }

        var loadedModel: MLModel?
        let modelResult = CemuBridge_PerformBlockSafely {
            loadedModel = coreMLModel(quality: quality)
        }
        guard modelResult, let model = loadedModel else { return nil }
        guard let input = modelInput(from: source) else { return nil }

        var multiArrayOutput: MLMultiArray?
        let predictResult = CemuBridge_PerformBlockSafely {
            if let provider = try? MLDictionaryFeatureProvider(dictionary: ["input": input]),
               let prediction = try? model.prediction(from: provider) {
                multiArrayOutput = prediction.featureValue(for: "output")?.multiArrayValue
            }
        }
        guard predictResult, let output = multiArrayOutput else {
            return nil
        }

        guard let result = image(from: output),
              let enlarged = scale(CIImage(cgImage: result), by: CGFloat(targetDimension) / CGFloat(result.width)) else { return nil }
        return context.createCGImage(enlarged, from: enlarged.extent.integral)
    }

    @available(iOS 18.0, *)
    private static func coreMLModel(quality: ArtworkQuality) -> MLModel? {
        modelStorage.lock.lock()
        defer { modelStorage.lock.unlock() }

        if modelStorage.quality != quality {
            modelStorage.model = nil
            modelStorage.attemptedLoad = false
            modelStorage.quality = quality
        }
        if let model = modelStorage.model {
            return model
        }
        guard !modelStorage.attemptedLoad else { return nil }

        let candidateNames: [String]
        switch quality {
        case .highQuality:
            candidateNames = ["Artwork-quality", "Artwork-normal"]
        case .normalQuality:
            candidateNames = ["Artwork-normal", "Artwork-quality"]
        }

        var targetURL: URL?
        for name in candidateNames {
            if let url = modelURL(named: name) {
                targetURL = url
                break
            }
        }

        guard let targetURL, let preparedModel = ArtworkModelArchive.prepare(targetURL) else {
            return nil
        }

        let configuration = MLModelConfiguration()
        configuration.computeUnits = .cpuAndGPU
        var createdModel: MLModel?
        let success = CemuBridge_PerformBlockSafely {
            createdModel = try? MLModel(contentsOf: preparedModel, configuration: configuration)
        }
        if success, let model = createdModel {
            modelStorage.model = model
            modelStorage.attemptedLoad = true
            return model
        } else {
            // Attempt normal quality fallback if high quality failed to load
            if quality == .highQuality {
                let fallbackNames = ["Artwork-normal"]
                for fbName in fallbackNames {
                    let fbURL = modelURL(named: fbName)
                    if let fbURL, let preparedURL = ArtworkModelArchive.prepare(fbURL) {
                        var fbLoaded: MLModel?
                        let fbSuccess = CemuBridge_PerformBlockSafely {
                            fbLoaded = try? MLModel(contentsOf: preparedURL, configuration: configuration)
                        }
                        if fbSuccess, let fallbackModel = fbLoaded {
                            modelStorage.model = fallbackModel
                            modelStorage.attemptedLoad = true
                            return fallbackModel
                        }
                    }
                }
            }
            modelStorage.attemptedLoad = false
            return nil
        }
    }

    private static func modelInput(from source: CGImage) -> MLMultiArray? {
        let dimension = [128, 256].contains(source.width) ? source.width : (source.width <= 192 ? 128 : 256)
        var pixels = [UInt8](repeating: 0, count: dimension * dimension * 4)
        let rendered = pixels.withUnsafeMutableBytes { bytes -> Bool in
            guard let context = CGContext(
                data: bytes.baseAddress,
                width: dimension,
                height: dimension,
                bitsPerComponent: 8,
                bytesPerRow: dimension * 4,
                space: colorSpace,
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
            ) else {
                return false
            }
            context.interpolationQuality = .high
            context.draw(source, in: CGRect(x: 0, y: 0, width: dimension, height: dimension))
            return true
        }
        guard rendered,
              let array = try? MLMultiArray(
                shape: [1, 3, NSNumber(value: dimension), NSNumber(value: dimension)],
                dataType: .float16
              ) else {
            return nil
        }

        let planeSize = dimension * dimension
        let pointer = array.dataPointer.bindMemory(to: Float16.self, capacity: planeSize * 3)
        for pixelIndex in 0..<planeSize {
            let byteIndex = pixelIndex * 4
            pointer[pixelIndex] = Float16(Float(pixels[byteIndex]) / 255)
            pointer[planeSize + pixelIndex] = Float16(Float(pixels[byteIndex + 1]) / 255)
            pointer[planeSize * 2 + pixelIndex] = Float16(Float(pixels[byteIndex + 2]) / 255)
        }
        return array
    }

    private static func image(from array: MLMultiArray) -> CGImage? {
        let shape = array.shape.map(\.intValue)
        guard shape.count == 4, shape[0] == 1, shape[1] == 3,
              shape[2] == shape[3], [512, 1024].contains(shape[2]) else { return nil }
        let dimension = shape[2]
        let strides = array.strides.map(\.intValue)
        let planeSize = dimension * dimension
        var pixels = [UInt8](repeating: 255, count: planeSize * 4)
        func fill<T: BinaryFloatingPoint>(_ pointer: UnsafePointer<T>) {
            for y in 0..<dimension {
                for x in 0..<dimension {
                    let position = y * strides[2] + x * strides[3]
                    let destination = (y * dimension + x) * 4
                    for channel in 0..<3 {
                        pixels[destination + channel] = byte(from: Float(pointer[position + channel * strides[1]]))
                    }
                }
            }
        }
        switch array.dataType {
        case .float16: fill(UnsafePointer(array.dataPointer.assumingMemoryBound(to: Float16.self)))
        case .float32: fill(UnsafePointer(array.dataPointer.assumingMemoryBound(to: Float.self)))
        default: return nil
        }

        guard let provider = CGDataProvider(data: Data(pixels) as CFData) else { return nil }
        return CGImage(
            width: dimension,
            height: dimension,
            bitsPerComponent: 8,
            bitsPerPixel: 32,
            bytesPerRow: dimension * 4,
            space: colorSpace,
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
            provider: provider,
            decode: nil,
            shouldInterpolate: true,
            intent: .defaultIntent
        )
    }

    private static func byte(from value: Float) -> UInt8 {
        UInt8((max(0, min(1, value)) * 255).rounded())
    }

    private static func fallbackUpscale(_ source: CGImage) -> CGImage? {
        let longestEdge = CGFloat(max(source.width, source.height))
        guard longestEdge > 0 else { return nil }

        let sourceImage = CIImage(cgImage: source)
        let scaleFactor = CGFloat(targetDimension) / longestEdge
        guard let enlarged = scale(sourceImage, by: scaleFactor) else { return nil }

        // A restrained fallback keeps original colors and avoids ringing around outlines.
        let detail = CIFilter.unsharpMask()
        detail.inputImage = enlarged
        detail.radius = 1.0
        detail.intensity = 0.25
        let output = (detail.outputImage ?? enlarged).cropped(to: enlarged.extent)
        return context.createCGImage(output, from: output.extent.integral)
    }

    private static func preserveSourceShading(source: CGImage, neural: CGImage) -> CGImage? {
        let bounds = CGRect(x: 0, y: 0, width: source.width, height: source.height)
        guard source.width == neural.width, source.height == neural.height else { return source }
        let original = CIImage(cgImage: source)
        let enhanced = CIImage(cgImage: neural)
        let count = source.width * source.height * 4
        func pixels(_ image: CIImage) -> [UInt8] {
            var result = [UInt8](repeating: 0, count: count)
            result.withUnsafeMutableBytes { bytes in
                context.render(image, toBitmap: bytes.baseAddress!, rowBytes: source.width * 4,
                               bounds: bounds, format: .RGBA8, colorSpace: colorSpace)
            }
            return result
        }
        var output = pixels(original)
        let ai = pixels(enhanced)
        let sourceShading = pixels(original.clampedToExtent().applyingGaussianBlur(sigma: 6).cropped(to: bounds))
        let aiShading = pixels(enhanced.clampedToExtent().applyingGaussianBlur(sigma: 6).cropped(to: bounds))
        for pixel in stride(from: 0, to: count, by: 4) {
            for channel in 0..<3 {
                let i = pixel + channel
                let shadingDelta = Float(aiShading[i]) - Float(sourceShading[i])
                // Preserve the model's sharp edge/detail signal while correcting broad
                // color and lighting drift toward the original artwork.
                let value = Float(ai[i]) - 0.75 * shadingDelta
                output[i] = UInt8(max(0, min(255, value.rounded())))
            }
            // Keep the source alpha channel unchanged.
        }
        guard let provider = CGDataProvider(data: Data(output) as CFData) else { return nil }
        return CGImage(width: source.width, height: source.height, bitsPerComponent: 8,
                       bitsPerPixel: 32, bytesPerRow: source.width * 4, space: colorSpace,
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    }

    private static func scale(_ image: CIImage, by factor: CGFloat) -> CIImage? {
        let lanczos = CIFilter.lanczosScaleTransform()
        lanczos.inputImage = image
        lanczos.scale = Float(factor)
        lanczos.aspectRatio = 1
        return lanczos.outputImage
    }

    private static func cacheKey(for iconURL: URL) -> String? {
        // The sandbox UUID, folder name, timestamps and app/model versions may
        // change during updates. Only changed source artwork requires new work.
        guard let bytes = try? Data(contentsOf: iconURL, options: .mappedIfSafe), !bytes.isEmpty else { return nil }
        return SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined()
    }

    private static func legacyCacheKey(for iconURL: URL) -> String {
        let attributes = try? FileManager.default.attributesOfItem(atPath: iconURL.path)
        let size = (attributes?[.size] as? NSNumber)?.uint64Value ?? 0
        let modified = (attributes?[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0
        let source = "\(algorithmVersion)|\(ArtworkQuality.current.rawValue)|\(iconURL.standardizedFileURL.path)|\(size)|\(modified)"
        return String(format: "%016llx", fnv1a64(source.utf8))
    }

    private static func fnv1a64<S: Sequence>(_ bytes: S) -> UInt64 where S.Element == UInt8 {
        var hash: UInt64 = 14_695_981_039_346_656_037
        for byte in bytes {
            hash ^= UInt64(byte)
            hash &*= 1_099_511_628_211
        }
        return hash
    }

    private static func cacheDirectory(create: Bool) -> URL? {
        guard let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first else {
            return nil
        }
        // Completed covers are user-visible generated assets, not disposable
        // caches. Keep this directory unversioned so app updates reuse them.
        let directory = support.appendingPathComponent("LibraryArtwork/Generated", isDirectory: true)
        if create {
            do { try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true) }
            catch { return nil }
        }
        return directory
    }

    private static func cachedImage(for key: String, legacyKey: String? = nil) -> UIImage? {
        if let url = cacheDirectory(create: false)?.appendingPathComponent(key).appendingPathExtension("png"),
           let image = UIImage(contentsOfFile: url.path) {
            return image
        }
        // Promote identifiable v6 covers without another Core ML inference.
        // Leave the old files intact if writing the persistent copy fails.
        if let legacyKey,
           let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first {
            let old = caches.appendingPathComponent("LibraryArtwork/v6", isDirectory: true)
                .appendingPathComponent(legacyKey).appendingPathExtension("png")
            if let image = UIImage(contentsOfFile: old.path) {
                save(image, for: key)
                return image
            }
        }
        return nil
    }

    private static func save(_ image: UIImage, for key: String) {
        guard let data = image.pngData(),
              let url = cacheDirectory(create: true)?.appendingPathComponent(key).appendingPathExtension("png") else {
            return
        }
        try? data.write(to: url, options: .atomic)
    }
}
