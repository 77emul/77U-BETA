import SwiftUI
import UIKit
import UniformTypeIdentifiers

private extension UTType {
    static let wiiUArchive = UTType(importedAs: "org.cemu.wua", conformingTo: .data)
    static let wiiUDiscImage = UTType(importedAs: "org.cemu.wud", conformingTo: .data)
    static let compressedWiiUDiscImage = UTType(importedAs: "org.cemu.wux", conformingTo: .data)
}

private let wiiUGameLibraryContentTypes: [UTType] = [
    .folder, .wiiUArchive, .wiiUDiscImage, .compressedWiiUDiscImage
]

private func stringFromNullTerminatedBuffer(_ buffer: [CChar]) -> String {
    let bytes = buffer.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }
    return String(decoding: bytes, as: Unicode.UTF8.self)
}

/// Safely extracts a null-terminated UTF-8 string from a fixed-size C tuple buffer.
/// Does not use pointer casting, does not escape closures, and is strictly bounded by buffer size.
func cString<T>(_ tuple: T) -> String {
    var copy = tuple
    return withUnsafeBytes(of: &copy) { rawBuffer in
        guard let baseAddress = rawBuffer.baseAddress else { return "" }
        let count = rawBuffer.count
        if count == 0 { return "" }
        var len = 0
        while len < count && rawBuffer[len] != 0 {
            len += 1
        }
        let byteBuffer = UnsafeRawBufferPointer(start: baseAddress, count: len)
        return String(decoding: byteBuffer, as: UTF8.self)
    }
}

@MainActor
final class BridgeViewModel: ObservableObject {
    @Published var bridgeInfo: CemuBridgeInfo = CemuBridge_GetInfo()
    @Published var securityScopeStatus: String = "Not Required"
    @Published var isJITWarningPresented: Bool = false

    @Published var isLaunching = false
    @Published var isPreparingJIT = false
    @Published var launchError: String?
    private var jitPreparationTask: Task<Bool, Never>?

    private var activeSecurityScopedUrl: URL?
    private var isSecurityScopeActive: Bool = false

    func refresh() {
        guard !isLaunching, !isPreparingJIT else { return }
        self.bridgeInfo = CemuBridge_GetInfo()
    }

    @discardableResult
    func launchTitle(from selectedUrl: URL, prepareArtwork: () async -> Void) async -> Bool {
        guard !isLaunching, !bridgeInfo.titleBootResult.titleLoaded else { return false }
        launchError = nil
        isLaunching = true
        defer {
            isLaunching = false
            refresh()
            ExternalDisplayCoordinator.shared.isGameRunning = bridgeInfo.titleBootResult.titleLoaded
        }
        if CemuBridge_GetCPUDiagnosticMode() != 1 {
            if CemuBridge_IsXcodeDebuggerAttached() {
                launchError = "Xcode's debugger is connected. To use JIT, turn off Debug executable in Edit Scheme -> Run, or select the interpreter CPU mode."
                return false
            }
            guard await JITCoordinator.shared.ensureReady() else {
                launchError = JITCoordinator.shared.error
                return false
            }
            // A Play tap during warmup must wait for the same reservation, not vanish
            // or enter the native allocator concurrently with it.
            if let preparation = jitPreparationTask {
                let prepared = await preparation.value
                isPreparingJIT = false
                guard prepared else {
                    launchError = "JIT memory could not be prepared. Reconnect Cemu in StikDebug and restart the app."
                    return false
                }
            }
        }
        await prepareArtwork()

        var isDirectory: ObjCBool = false
        let fileManager = FileManager.default
        _ = fileManager.fileExists(atPath: selectedUrl.path, isDirectory: &isDirectory)

        if isDirectory.boolValue {
            // Extracted Wii U Title Root Directory (HOST_FS: code/, content/, meta/)
            if activeSecurityScopedUrl != nil {
                print("Warning: A security-scoped directory is already active for this session.")
            }

            let started = selectedUrl.startAccessingSecurityScopedResource()
            if started {
                activeSecurityScopedUrl = selectedUrl
                isSecurityScopeActive = true
                securityScopeStatus = "Yes"
                CemuBridge_SetSecurityScopeStatus("Yes")
            } else if fileManager.isReadableFile(atPath: selectedUrl.path) {
                isSecurityScopeActive = false
                securityScopeStatus = "Not Required"
                CemuBridge_SetSecurityScopeStatus("Not Required")
            } else {
                securityScopeStatus = "No"
                CemuBridge_SetSecurityScopeStatus("No")
                launchError = "The game folder is no longer accessible. Select it again in Add & Manage."
                return false
            }

            CemuBridge_LogJITMilestone("[JIT] launchTitle (directory): scope=\(securityScopeStatus), path=\(selectedUrl.lastPathComponent)")

            let localPath = selectedUrl.path
            CemuBridge_LogJITMilestone("[JIT] Invoking CemuBridge_LaunchTitle for directory: \(selectedUrl.lastPathComponent)")
            return await performLaunch(path: localPath)
        } else {
            let extensionName = selectedUrl.pathExtension.lowercased()
            let isGameArchive = ["wua", "wud", "wux"].contains(extensionName)
            let accessing = selectedUrl.startAccessingSecurityScopedResource()

            if isGameArchive {
                if accessing {
                    activeSecurityScopedUrl = selectedUrl
                    isSecurityScopeActive = true
                    securityScopeStatus = "Yes"
                } else if fileManager.isReadableFile(atPath: selectedUrl.path) {
                    isSecurityScopeActive = false
                    securityScopeStatus = "Not Required"
                } else {
                    securityScopeStatus = "No"
                    CemuBridge_SetSecurityScopeStatus("No")
                    launchError = "The WUA file is no longer accessible. Select it again in Add & Manage."
                    return false
                }
                CemuBridge_SetSecurityScopeStatus(securityScopeStatus)
                CemuBridge_LogJITMilestone("[JIT] launchTitle (\(extensionName.uppercased())): scope=\(securityScopeStatus), path=\(selectedUrl.lastPathComponent)")
                return await performLaunch(path: selectedUrl.path)
            }

            // Standalone RPX (Sandbox Copy to Caches/titles/)
            securityScopeStatus = "Not Required"
            CemuBridge_SetSecurityScopeStatus("Not Required")
            defer {
                if accessing {
                    selectedUrl.stopAccessingSecurityScopedResource()
                }
            }

            CemuBridge_LogJITMilestone("[JIT] launchTitle (standalone RPX): \(selectedUrl.lastPathComponent)")

            do {
                guard let cachesDir = fileManager.urls(for: .cachesDirectory, in: .userDomainMask).first else {
                    CemuBridge_LogJITMilestone("[JIT] File import failed: Unable to locate Caches directory")
                    return false
                }
                let titlesDir = cachesDir.appendingPathComponent("titles", isDirectory: true)
                try fileManager.createDirectory(at: titlesDir, withIntermediateDirectories: true, attributes: nil)

                let destUrl = titlesDir.appendingPathComponent(selectedUrl.lastPathComponent)
                try await Task.detached(priority: .userInitiated) {
                    let files = FileManager.default
                    if selectedUrl.standardizedFileURL != destUrl.standardizedFileURL {
                        if files.fileExists(atPath: destUrl.path) { try files.removeItem(at: destUrl) }
                        try files.copyItem(at: selectedUrl, to: destUrl)
                    }
                }.value

                let localPath = destUrl.path
                CemuBridge_LogJITMilestone("[JIT] Invoking CemuBridge_LaunchTitle for RPX: \(destUrl.lastPathComponent)")
                return await performLaunch(path: localPath)
            } catch {
                CemuBridge_LogJITMilestone("[JIT] File import/copy failed: \(error.localizedDescription)")
                launchError = error.localizedDescription
                return false
            }
        }
    }

    private func performLaunch(path: String) async -> Bool {
        let result = await Task.detached(priority: .userInitiated) {
            path.withCString { CemuBridge_LaunchTitle($0) }
        }.value
        guard result.launchRequested else {
            if isSecurityScopeActive, let url = activeSecurityScopedUrl {
                url.stopAccessingSecurityScopedResource()
                activeSecurityScopedUrl = nil
                isSecurityScopeActive = false
            }
            launchError = cString(result.errorMessage)
            if launchError?.isEmpty != false { launchError = "The game could not be started." }
            return false
        }
        return true
    }

    func stopCurrentGame() {
        CemuBridge_StopTitle()
        if isSecurityScopeActive, let url = activeSecurityScopedUrl {
            url.stopAccessingSecurityScopedResource()
            activeSecurityScopedUrl = nil
            isSecurityScopeActive = false
        }
        refresh()
        ExternalDisplayCoordinator.shared.isGameRunning = false
        GameOrientationCoordinator.setGameActive(false)
    }

    deinit {
        if isSecurityScopeActive, let url = activeSecurityScopedUrl {
            url.stopAccessingSecurityScopedResource()
        }
    }
}

struct GameLibraryEntry: Identifiable {
    let relativePath: String
    let isGameFile: Bool
    let title: String
    let region: String?
    let productCode: String?
    let titleID: String?
    let version: UInt16?
    let hasUpdate: Bool
    let dlcVersion: UInt16?
    var icon: UIImage?
    var artworkCacheKey: String?
    var artworkNeedsEnhancement: Bool
    var lastPlayed: Date?

    var id: String { relativePath }
    var numericTitleID: UInt64? {
        guard let titleID else { return nil }
        return UInt64(titleID, radix: 16)
    }
}

private struct GameLibraryMetadata {
    let title: String?
    let region: String?
    let productCode: String?
    let titleID: String?
    let version: UInt16?
}

private final class GameLibraryXMLCollector: NSObject, XMLParserDelegate {
    private(set) var values: [String: String] = [:]
    private var currentElement: String?
    private var currentValue = ""

    func parser(_ parser: XMLParser, didStartElement elementName: String,
                namespaceURI: String?, qualifiedName qName: String?,
                attributes attributeDict: [String: String] = [:]) {
        currentElement = elementName.lowercased()
        currentValue = ""
    }

    func parser(_ parser: XMLParser, foundCharacters string: String) {
        guard currentElement != nil else { return }
        currentValue += string
    }

    func parser(_ parser: XMLParser, didEndElement elementName: String,
                namespaceURI: String?, qualifiedName qName: String?) {
        let key = elementName.lowercased()
        let value = currentValue.trimmingCharacters(in: .whitespacesAndNewlines)
        if !value.isEmpty {
            values[key] = value
        }
        currentElement = nil
        currentValue = ""
    }
}

fileprivate enum GameLibraryMetadataReader {
    static func read(from titleURL: URL) -> GameLibraryMetadata {
        let metaValues = xmlValues(at: titleURL.appendingPathComponent("meta/meta.xml"))
        let appValues = xmlValues(at: titleURL.appendingPathComponent("code/app.xml"))
        return metadata(metaValues: metaValues, appValues: appValues, preferredLanguages: ["en"])
    }

    static func metadata(metaValues: [String: String], appValues: [String: String],
                         preferredLanguages: [String]) -> GameLibraryMetadata {
        let languageKeys = preferredLanguages.compactMap(languageKey(for:)) + ["en"]
        let localizedTitle = languageKeys.lazy
            .compactMap { metaValues["longname_\($0)"] }
            .first(where: { !$0.isEmpty })
            ?? languageKeys.lazy.compactMap { metaValues["shortname_\($0)"] }.first(where: { !$0.isEmpty })
            ?? metaValues.first(where: { $0.key.hasPrefix("longname_") && !$0.value.isEmpty })?.value

        let title = localizedTitle?
            .replacingOccurrences(of: "\r", with: " ")
            .replacingOccurrences(of: "\n", with: " ")
            .trimmingCharacters(in: .whitespacesAndNewlines)

        let region = metaValues["region"].flatMap(regionName)
        let productCode = trimmedValue(metaValues["product_code"])
        let titleID = normalizedTitleID(appValues["title_id"] ?? metaValues["title_id"])
        let version = appValues["title_version"].flatMap {
            UInt16($0.trimmingCharacters(in: .whitespacesAndNewlines), radix: 16)
        }

        return GameLibraryMetadata(
            title: title?.isEmpty == false ? title : nil,
            region: region,
            productCode: productCode,
            titleID: titleID,
            version: version
        )
    }

    private static func xmlValues(at url: URL) -> [String: String] {
        guard let data = try? Data(contentsOf: url, options: .mappedIfSafe) else { return [:] }
        let collector = GameLibraryXMLCollector()
        let parser = XMLParser(data: data)
        parser.delegate = collector
        guard parser.parse() else { return [:] }
        return collector.values
    }

    private static func languageKey(for identifier: String) -> String? {
        let normalized = identifier.lowercased().replacingOccurrences(of: "_", with: "-")
        if normalized.hasPrefix("zh-hans") || normalized.hasPrefix("zh-cn") {
            return "zhs"
        }
        if normalized.hasPrefix("zh-hant") || normalized.hasPrefix("zh-tw") {
            return "zht"
        }

        switch normalized.split(separator: "-").first.map(String.init) {
        case "ja": return "ja"
        case "en": return "en"
        case "fr": return "fr"
        case "de": return "de"
        case "it": return "it"
        case "es": return "es"
        case "ko": return "ko"
        case "nl": return "nl"
        case "pt": return "pt"
        case "ru": return "ru"
        default: return nil
        }
    }

    private static func regionName(_ rawValue: String) -> String? {
        guard let value = UInt32(rawValue.trimmingCharacters(in: .whitespacesAndNewlines)) else { return nil }
        switch value {
        case 0x1: return "Japan"
        case 0x2: return "USA"
        case 0x4: return "Europe"
        case 0x8: return "Australia"
        case 0x10: return "China"
        case 0x20: return "Korea"
        case 0x40: return "Taiwan"
        case 0xFF: return "All regions"
        default: return value == 0 ? nil : "Multi-region"
        }
    }

    private static func normalizedTitleID(_ rawValue: String?) -> String? {
        guard let rawValue else { return nil }
        let compact = rawValue
            .replacingOccurrences(of: "0x", with: "", options: .caseInsensitive)
            .filter { $0.isHexDigit }
        guard !compact.isEmpty, let value = UInt64(compact, radix: 16) else { return nil }
        return String(format: "%016llX", value)
    }

    private static func trimmedValue(_ value: String?) -> String? {
        guard let value else { return nil }
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }
}

enum GameLibraryIconReader {
    static func readIcon(at iconURL: URL) -> UIImage? {
        guard let data = try? Data(contentsOf: iconURL, options: .mappedIfSafe) else { return nil }
        return decodeUncompressedTGA(data)
    }

    static func decodeUncompressedTGA(_ data: Data, maximumDimension: Int = 1024) -> UIImage? {
        let bytes = [UInt8](data)
        guard bytes.count >= 18,
              bytes[1] == 0,
              bytes[2] == 2 else {
            return nil
        }

        let width = Int(bytes[12]) | (Int(bytes[13]) << 8)
        let height = Int(bytes[14]) | (Int(bytes[15]) << 8)
        let bitsPerPixel = Int(bytes[16])
        guard width > 0, height > 0,
              width <= maximumDimension, height <= maximumDimension,
              bitsPerPixel == 24 || bitsPerPixel == 32 else {
            return nil
        }

        let bytesPerPixel = bitsPerPixel / 8
        let pixelOffset = 18 + Int(bytes[0])
        let pixelCount = width * height
        guard pixelCount <= (bytes.count - pixelOffset) / bytesPerPixel else { return nil }

        let originAtTop = (bytes[17] & 0x20) != 0
        let originAtRight = (bytes[17] & 0x10) != 0
        var rgba = [UInt8](repeating: 0, count: pixelCount * 4)

        for sourceY in 0..<height {
            let destinationY = originAtTop ? sourceY : height - sourceY - 1
            for sourceX in 0..<width {
                let destinationX = originAtRight ? width - sourceX - 1 : sourceX
                let sourceOffset = pixelOffset + (sourceY * width + sourceX) * bytesPerPixel
                let destinationOffset = (destinationY * width + destinationX) * 4
                rgba[destinationOffset] = bytes[sourceOffset + 2]
                rgba[destinationOffset + 1] = bytes[sourceOffset + 1]
                rgba[destinationOffset + 2] = bytes[sourceOffset]
                rgba[destinationOffset + 3] = bytesPerPixel == 4 ? bytes[sourceOffset + 3] : 255
            }
        }

        guard let provider = CGDataProvider(data: Data(rgba) as CFData),
              let image = CGImage(
                width: width,
                height: height,
                bitsPerComponent: 8,
                bitsPerPixel: 32,
                bytesPerRow: width * 4,
                space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
                provider: provider,
                decode: nil,
                shouldInterpolate: true,
                intent: .defaultIntent
              ) else {
            return nil
        }

        return UIImage(cgImage: image)
    }
}

struct ArtworkEnhancementProgress: Equatable {
    let completed: Int
    let total: Int
    let isPaused: Bool

    var fraction: Double {
        guard total > 0 else { return 0 }
        return Double(completed) / Double(total)
    }
}

final class GameLibraryStore: ObservableObject, @unchecked Sendable {
    @Published private(set) var entries: [GameLibraryEntry] = []
    @Published private(set) var folderName: String?
    @Published private(set) var statusMessage = "Choose a Wii U game folder or a WUA file."
    @Published private(set) var artworkProgress: ArtworkEnhancementProgress?
    @Published private(set) var defaultFolderName = UserDefaults.standard.string(forKey: "CemuIOS.GameLibrary.DefaultFolderName") ?? "Last opened location"
    @Published private(set) var defaultFolderError: String?

    private static let defaultBookmarkKey = "CemuIOS.GameLibrary.DefaultFolderBookmark"
    private static let defaultConfiguredKey = "CemuIOS.GameLibrary.DefaultFolderConfigured"
    private static let defaultNameKey = "CemuIOS.GameLibrary.DefaultFolderName"

    var hasDefaultGamesFolder: Bool {
        UserDefaults.standard.bool(forKey: Self.defaultConfiguredKey)
    }

    private let bookmarkKey = "CemuIOS.GameLibrary.Bookmark"
    private let lastPlayedKey = "CemuIOS.GameLibrary.LastPlayed"
    private var rootURL: URL?
    private var ownsSecurityScope = false
    private var artworkLifecycle = ArtworkEnhancementLifecycle()
    private let artworkQueue: OperationQueue = {
        let queue = OperationQueue()
        queue.name = "com.cemu.ios.library-artwork"
        queue.qualityOfService = .utility
        queue.maxConcurrentOperationCount = 1
        return queue
    }()
    private var artworkGeneration = UUID()

    deinit {
        artworkQueue.cancelAllOperations()
        releaseSecurityScope()
    }

    func restoreLibraryFolder() {
        // Keep the app-owned folder available in Files even when the user has
        // selected and bookmarked a different library folder.
        _ = try? ensureAppFolders()

        guard rootURL == nil else { return }
        if hasDefaultGamesFolder {
            openDefaultGamesFolder()
            return
        }
        guard let bookmark = UserDefaults.standard.data(forKey: bookmarkKey) else {
            useAppGamesFolder(asDefault: false)
            return
        }

        do {
            var isStale = false
            let url = try URL(
                resolvingBookmarkData: bookmark,
                options: [],
                relativeTo: nil,
                bookmarkDataIsStale: &isStale
            )
            try activateLibraryFolder(url, persistBookmark: isStale)
        } catch {
            UserDefaults.standard.removeObject(forKey: bookmarkKey)
            useAppGamesFolder(asDefault: false)
        }
    }

    func useAppGamesFolder(asDefault: Bool = true) {
        do {
            try activateAppGamesFolder()
            if asDefault {
                UserDefaults.standard.removeObject(forKey: bookmarkKey)
                UserDefaults.standard.removeObject(forKey: Self.defaultBookmarkKey)
                UserDefaults.standard.set(true, forKey: Self.defaultConfiguredKey)
                UserDefaults.standard.set("77U -> Games", forKey: Self.defaultNameKey)
                defaultFolderName = "77U -> Games"
            }
            defaultFolderError = nil
        } catch {
            statusMessage = "Could not prepare the 77U Games folder: \(error.localizedDescription)"
            defaultFolderError = statusMessage
        }
    }

    private func activateAppGamesFolder() throws {
        let gamesURL = try ensureAppFolders()
        try activateLibraryFolder(gamesURL, persistBookmark: false, requireDirectory: true)
        if entries.isEmpty {
            statusMessage = "Put extracted Wii U game folders or WUA files in On My iPhone or iPad -> 77U -> Games, then reopen the default folder."
        }
    }

    func setDefaultGamesFolder(_ url: URL) throws {
        let bookmark = try activateLibraryFolder(url, persistBookmark: true, requireDirectory: true)
        UserDefaults.standard.set(bookmark, forKey: Self.defaultBookmarkKey)
        UserDefaults.standard.set(true, forKey: Self.defaultConfiguredKey)
        UserDefaults.standard.set(url.lastPathComponent, forKey: Self.defaultNameKey)
        defaultFolderName = url.lastPathComponent
        defaultFolderError = nil
    }

    func openDefaultGamesFolder() {
        guard hasDefaultGamesFolder else {
            if rootURL == nil { restoreLibraryFolder() }
            else { rescan() }
            return
        }
        defaultFolderError = nil
        do {
            if let bookmark = UserDefaults.standard.data(forKey: Self.defaultBookmarkKey) {
                var isStale = false
                let url = try URL(resolvingBookmarkData: bookmark, options: [], relativeTo: nil,
                                  bookmarkDataIsStale: &isStale)
                let refreshed = try activateLibraryFolder(url, persistBookmark: isStale, requireDirectory: true)
                if let refreshed { UserDefaults.standard.set(refreshed, forKey: Self.defaultBookmarkKey) }
                defaultFolderName = url.lastPathComponent
                UserDefaults.standard.set(defaultFolderName, forKey: Self.defaultNameKey)
            } else {
                try activateAppGamesFolder()
            }
        } catch {
            // A disconnected drive or revoked grant must not erase the user's default.
            let message = "The default folder \"\(defaultFolderName)\" is unavailable. Reconnect it and tap Open default folder, or choose the folder again."
            if rootURL == nil { try? activateAppGamesFolder() }
            defaultFolderError = message
            statusMessage = message
        }
    }

    private func ensureAppFolders() throws -> URL {
        guard let documentsURL = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else {
            throw CocoaError(.fileNoSuchFile)
        }
        let gamesURL = documentsURL.appendingPathComponent("Games", isDirectory: true)
        let updatesURL = documentsURL.appendingPathComponent("Updates", isDirectory: true)
        try FileManager.default.createDirectory(at: gamesURL, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: updatesURL, withIntermediateDirectories: true)
        return gamesURL
    }

    func selectLibraryLocation(_ url: URL) {
        do {
            try activateLibraryFolder(url, persistBookmark: true)
        } catch {
            statusMessage = "Could not open the selected game location: \(error.localizedDescription)"
        }
    }

    func rescan() {
        guard let rootURL else { return }
        rootURL.path.withCString { cPath in
            CemuBridge_AddGameScanPath(cPath)
        }
        let lastPlayed = UserDefaults.standard.dictionary(forKey: lastPlayedKey) as? [String: Double] ?? [:]
        entries = Self.discoverGames(in: rootURL, lastPlayed: lastPlayed)
        scheduleArtworkEnhancement()
        CemuBridge_RescanTitles()
        statusMessage = entries.isEmpty
            ? "No extracted Wii U games or valid WUA files were found here."
            : "Found \(entries.count) game\(entries.count == 1 ? "" : "s")."
    }

    func url(for entry: GameLibraryEntry) -> URL? {
        if entry.relativePath.hasPrefix("__system_title__:") {
            let subpath = String(entry.relativePath.dropFirst("__system_title__:".count))
            guard let documentsURL = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else { return nil }
            return documentsURL.appendingPathComponent("mlc01/sys/title/\(subpath)", isDirectory: true)
        }
        guard let rootURL else { return nil }
        if entry.relativePath == "." {
            return rootURL
        }
        return rootURL.appendingPathComponent(entry.relativePath, isDirectory: !entry.isGameFile)
    }

    func markPlayed(_ entry: GameLibraryEntry) {
        let playedAt = Date()
        var values = UserDefaults.standard.dictionary(forKey: lastPlayedKey) as? [String: Double] ?? [:]
        values[entry.relativePath] = playedAt.timeIntervalSince1970
        UserDefaults.standard.set(values, forKey: lastPlayedKey)
        if let index = entries.firstIndex(where: { $0.id == entry.id }) {
            entries[index].lastPlayed = playedAt
        }
    }

    func suspendArtworkEnhancement(waitUntilIdle: Bool = false) {
        artworkGeneration = UUID()
        artworkQueue.cancelAllOperations()
        if waitUntilIdle {
            artworkQueue.waitUntilAllOperationsAreFinished()
            GameArtworkEnhancer.releaseModel()
        }
        artworkProgress = nil
    }

    @MainActor
    func prepareForGameLaunch() async {
        suspendArtworkEnhancement()
        artworkLifecycle.prepareForGameLaunch()
        await withCheckedContinuation { continuation in
            artworkQueue.addBarrierBlock {
                GameArtworkEnhancer.releaseModel()
                continuation.resume()
            }
        }
    }

    func resumeArtworkEnhancementIfNeeded() {
        // Power and thermal notifications also arrive while a title is running.
        // They must not restart Core ML cover inference on the game's GPU.
        guard artworkLifecycle.canSchedule else { return }
        guard GameArtworkEnhancer.isEnabled else {
            suspendArtworkEnhancement()
            return
        }
        guard entries.contains(where: { $0.artworkNeedsEnhancement }) else { return }
        scheduleArtworkEnhancement()
    }

    func resumeArtworkEnhancementAfterGame() {
        artworkLifecycle.gameDidEnd()
        resumeArtworkEnhancementIfNeeded()
    }

    func handleArtworkMemoryWarning() {
        artworkLifecycle.receivedMemoryWarning()
        // Invalidate UI completions before canceling jobs. The current prediction
        // may finish, but it cannot publish its result or start another job.
        suspendArtworkEnhancement()
        artworkQueue.addBarrierBlock {
            GameArtworkEnhancer.releaseResourcesAfterMemoryWarning()
        }
    }

    func applyArtworkEnhancementPreference() {
        suspendArtworkEnhancement()
        artworkLifecycle.userChangedPreference()
        let useEnhancedArtwork = GameArtworkEnhancer.isEnabled

        for index in entries.indices {
            guard let titleURL = url(for: entries[index]) else { continue }
            let artwork = entries[index].isGameFile
                ? Self.archiveArtwork(from: titleURL, titleID: entries[index].titleID, useEnhancedCache: useEnhancedArtwork)
                : GameArtworkEnhancer.loadArtwork(from: titleURL, useEnhancedCache: useEnhancedArtwork)
            entries[index].icon = artwork.image
            entries[index].artworkCacheKey = artwork.cacheKey
            entries[index].artworkNeedsEnhancement = artwork.needsEnhancement
        }

        if useEnhancedArtwork {
            scheduleArtworkEnhancement()
        }
    }

    private func scheduleArtworkEnhancement() {
        guard artworkLifecycle.canSchedule else { return }
        artworkGeneration = UUID()
        artworkQueue.cancelAllOperations()

        guard GameArtworkEnhancer.isEnabled else {
            artworkProgress = nil
            return
        }

        let jobs = entries.compactMap { entry -> (id: String, key: String, image: CGImage)? in
            guard entry.artworkNeedsEnhancement,
                  let key = entry.artworkCacheKey,
                  let image = entry.icon?.cgImage else {
                return nil
            }
            return (entry.id, key, image)
        }

        guard !jobs.isEmpty else {
            artworkProgress = nil
            return
        }

        if GameArtworkEnhancer.shouldPauseForSystemPressure {
            artworkProgress = ArtworkEnhancementProgress(completed: 0, total: jobs.count, isPaused: true)
            return
        }

        let generation = artworkGeneration
        let quality = ArtworkQuality.current
        artworkProgress = ArtworkEnhancementProgress(completed: 0, total: jobs.count, isPaused: false)

        let operation = BlockOperation()
        operation.addExecutionBlock { [weak self, weak operation] in
            for (offset, job) in jobs.enumerated() {
                guard operation?.isCancelled == false else { return }
                guard !GameArtworkEnhancer.shouldPauseForSystemPressure else {
                    DispatchQueue.main.async { [weak self] in
                        guard let self, self.artworkGeneration == generation else { return }
                        self.artworkProgress = ArtworkEnhancementProgress(
                            completed: offset,
                            total: jobs.count,
                            isPaused: true
                        )
                    }
                    return
                }

                let enhanced = autoreleasepool {
                    GameArtworkEnhancer.enhance(job.image, cacheKey: job.key, quality: quality)
                }

                DispatchQueue.main.async { [weak self] in
                    guard let self, self.artworkGeneration == generation else { return }
                    if let enhanced,
                       let index = self.entries.firstIndex(where: { $0.id == job.id }) {
                        self.entries[index].icon = enhanced
                        self.entries[index].artworkNeedsEnhancement = false
                    }

                    let completed = offset + 1
                    self.artworkProgress = ArtworkEnhancementProgress(
                        completed: completed,
                        total: jobs.count,
                        isPaused: false
                    )
                    if completed == jobs.count {
                        DispatchQueue.main.asyncAfter(deadline: .now() + 1.1) { [weak self] in
                            guard let self, self.artworkGeneration == generation else { return }
                            self.artworkProgress = nil
                        }
                    }
                }
            }
        }
        artworkQueue.addOperation(operation)
    }

    @discardableResult
    private func activateLibraryFolder(_ url: URL, persistBookmark: Bool, requireDirectory: Bool = false) throws -> Data? {
        let started = url.startAccessingSecurityScopedResource()
        var committed = false
        defer {
            if started && !committed { url.stopAccessingSecurityScopedResource() }
        }
        guard FileManager.default.isReadableFile(atPath: url.path) else {
            throw CocoaError(.fileReadNoPermission)
        }
        if requireDirectory {
            guard try url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory == true else {
                throw NSError(domain: NSCocoaErrorDomain, code: NSFileReadUnknownError,
                              userInfo: [NSLocalizedDescriptionKey: "Choose a folder, rather than an individual game file."])
            }
            // Verify enumeration before replacing the current library or bookmark.
            _ = try FileManager.default.contentsOfDirectory(at: url, includingPropertiesForKeys: nil,
                                                          options: [.skipsHiddenFiles])
        }
        let bookmark = persistBookmark ? try url.bookmarkData(options: [], includingResourceValuesForKeys: nil,
                                                             relativeTo: nil) : nil

        releaseSecurityScope()
        rootURL = url
        ownsSecurityScope = started
        committed = true
        folderName = url.lastPathComponent

        if let bookmark {
            UserDefaults.standard.set(bookmark, forKey: bookmarkKey)
        }
        rescan()
        return bookmark
    }

    private func releaseSecurityScope() {
        if ownsSecurityScope {
            rootURL?.stopAccessingSecurityScopedResource()
        }
        ownsSecurityScope = false
        rootURL = nil
    }

    private static func discoverGames(in root: URL, lastPlayed: [String: Double]) -> [GameLibraryEntry] {
        let fileManager = FileManager.default
        var pending: [(url: URL, depth: Int)] = [(root, 0)]
        var nextCandidateIndex = 0
        var games: [GameLibraryEntry] = []
        let standardizedRoot = root.standardizedFileURL
        let rootPath = standardizedRoot.path.hasSuffix("/")
            ? standardizedRoot.path
            : standardizedRoot.path + "/"

        while nextCandidateIndex < pending.count {
            let candidate = pending[nextCandidateIndex]
            nextCandidateIndex += 1
            if isSupportedGameFile(candidate.url, fileManager: fileManager) {
                let relativePath: String
                let standardizedCandidate = candidate.url.standardizedFileURL
                if standardizedCandidate == standardizedRoot {
                    relativePath = "."
                } else {
                    relativePath = String(standardizedCandidate.path.dropFirst(rootPath.count))
                }

                let fileInfo = candidate.url.path.withCString { CemuBridge_InspectGameFile($0) }
                let titleID = fileInfo.success ? String(format: "%016llX", fileInfo.titleId) : nil
                let installedContent = fileInfo.success ? CemuBridge_GetInstalledContentInfo(fileInfo.titleId) : nil
                let titleName = fileInfo.success ? cString(fileInfo.titleName) : ""
                let playedAt = lastPlayed[relativePath].map(Date.init(timeIntervalSince1970:))
                let artwork = archiveArtwork(from: candidate.url, titleID: titleID)
                games.append(GameLibraryEntry(
                    relativePath: relativePath,
                    isGameFile: true,
                    title: titleName.isEmpty ? displayTitle(for: candidate.url) : titleName,
                    region: fileInfo.success ? regionName(fileInfo.region) : nil,
                    productCode: candidate.url.pathExtension.uppercased(),
                    titleID: titleID,
                    version: installedContent?.hasUpdate == true
                        ? installedContent?.updateVersion
                        : (fileInfo.success ? fileInfo.titleVersion : nil),
                    hasUpdate: installedContent?.hasUpdate == true,
                    dlcVersion: installedContent?.hasDLC == true ? installedContent?.dlcVersion : nil,
                    icon: artwork.image,
                    artworkCacheKey: artwork.cacheKey,
                    artworkNeedsEnhancement: artwork.needsEnhancement,
                    lastPlayed: playedAt
                ))
                continue
            }

            if isExtractedTitle(candidate.url, fileManager: fileManager) {
                let relativePath: String
                let standardizedCandidate = candidate.url.standardizedFileURL
                if standardizedCandidate == standardizedRoot {
                    relativePath = "."
                } else {
                    relativePath = String(standardizedCandidate.path.dropFirst(rootPath.count))
                }
                let playedAt = lastPlayed[relativePath].map(Date.init(timeIntervalSince1970:))
                let metadata = GameLibraryMetadataReader.read(from: candidate.url)
                let artwork = GameArtworkEnhancer.loadArtwork(from: candidate.url)
                let numericTitleID = metadata.titleID.flatMap { UInt64($0, radix: 16) }
                let installedContent = numericTitleID.map { CemuBridge_GetInstalledContentInfo($0) }
                games.append(GameLibraryEntry(
                    relativePath: relativePath,
                    isGameFile: false,
                    title: metadata.title ?? displayTitle(for: candidate.url),
                    region: metadata.region,
                    productCode: metadata.productCode,
                    titleID: metadata.titleID,
                    version: installedContent?.hasUpdate == true
                        ? installedContent?.updateVersion
                        : metadata.version,
                    hasUpdate: installedContent?.hasUpdate == true,
                    dlcVersion: installedContent?.hasDLC == true
                        ? installedContent?.dlcVersion
                        : nil,
                    icon: artwork.image,
                    artworkCacheKey: artwork.cacheKey,
                    artworkNeedsEnhancement: artwork.needsEnhancement,
                    lastPlayed: playedAt
                ))
                continue
            }

            guard candidate.depth < 2,
                  let children = try? fileManager.contentsOfDirectory(
                    at: candidate.url,
                    includingPropertiesForKeys: [.isDirectoryKey, .isRegularFileKey, .isHiddenKey],
                    options: [.skipsHiddenFiles]
                  ) else {
                continue
            }

            for child in children {
                let values = try? child.resourceValues(forKeys: [.isDirectoryKey, .isRegularFileKey, .isHiddenKey])
                if values?.isHidden != true && (values?.isDirectory == true || values?.isRegularFile == true) {
                    pending.append((child, candidate.depth + 1))
                }
            }
        }

        // Sync games to MLC so Wii U Menu sees all titles in library
        syncLibraryGamesToMlc(games: games, root: root)

        // Discover installed Wii U System Menu
        if let documentsURL = fileManager.urls(for: .documentDirectory, in: .userDomainMask).first {
            let sysTitleBase = documentsURL.appendingPathComponent("mlc01/sys/title/00050010", isDirectory: true)
            for regionCode in ["10040200", "10040100", "10040000"] {
                let menuURL = sysTitleBase.appendingPathComponent(regionCode, isDirectory: true)
                if isExtractedTitle(menuURL, fileManager: fileManager) {
                    let relKey = "__system_title__:00050010/\(regionCode)"
                    let playedAt = lastPlayed[relKey].map(Date.init(timeIntervalSince1970:))
                    let metadata = GameLibraryMetadataReader.read(from: menuURL)
                    let artwork = GameArtworkEnhancer.loadArtwork(from: menuURL)
                    games.append(GameLibraryEntry(
                        relativePath: relKey,
                        isGameFile: false,
                        title: "Wii U Menu",
                        region: metadata.region ?? (regionCode == "10040200" ? "Europe" : (regionCode == "10040100" ? "USA" : "Japan")),
                        productCode: "Wii U Menu",
                        titleID: "00050010" + regionCode.uppercased(),
                        version: metadata.version ?? 277,
                        hasUpdate: false,
                        dlcVersion: nil,
                        icon: artwork.image,
                        artworkCacheKey: artwork.cacheKey,
                        artworkNeedsEnhancement: artwork.needsEnhancement,
                        lastPlayed: playedAt
                    ))
                    break
                }
            }
        }

        return games.sorted {
            let order = $0.title.localizedStandardCompare($1.title)
            return order == .orderedSame ? $0.id < $1.id : order == .orderedAscending
        }
    }

    private static func archiveArtwork(from archiveURL: URL, titleID: String?, useEnhancedCache: Bool = GameArtworkEnhancer.isEnabled) -> GameLibraryArtwork {
        let cacheRoot = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first
            ?? FileManager.default.temporaryDirectory
        let stableName = (titleID ?? archiveURL.lastPathComponent)
            .replacingOccurrences(of: "/", with: "_")
        let iconURL = cacheRoot.appendingPathComponent("ImportedTitleIcons", isDirectory: true)
            .appendingPathComponent("\(stableName).tga")
        try? FileManager.default.createDirectory(at: iconURL.deletingLastPathComponent(),
                                                 withIntermediateDirectories: true)

        if !FileManager.default.fileExists(atPath: iconURL.path) {
            let extracted = archiveURL.path.withCString { source in
                iconURL.path.withCString { destination in
                    CemuBridge_ExtractGameFileIcon(source, destination)
                }
            }
            if !extracted { try? FileManager.default.removeItem(at: iconURL) }
        }
        return GameArtworkEnhancer.loadArtwork(fromIcon: iconURL, useEnhancedCache: useEnhancedCache)
    }

    private static func syncLibraryGamesToMlc(games: [GameLibraryEntry], root: URL) {
        let fileManager = FileManager.default
        guard let documentsURL = fileManager.urls(for: .documentDirectory, in: .userDomainMask).first else { return }
        let mlcUsrTitles = documentsURL.appendingPathComponent("mlc01/usr/title/00050000", isDirectory: true)
        try? fileManager.createDirectory(at: mlcUsrTitles, withIntermediateDirectories: true)

        for game in games {
            guard !game.isGameFile else { continue }
            guard let titleID = game.titleID, titleID.count == 16, titleID.uppercased().hasPrefix("00050000") else { continue }
            let lowID = String(titleID.suffix(8)).lowercased()
            let destTitleURL = mlcUsrTitles.appendingPathComponent(lowID, isDirectory: true)
            let gameSourceURL = game.relativePath == "." ? root : root.appendingPathComponent(game.relativePath, isDirectory: true)

            if gameSourceURL.standardizedFileURL == destTitleURL.standardizedFileURL { continue }

            try? fileManager.createDirectory(at: destTitleURL, withIntermediateDirectories: true)
            for sub in ["code", "content", "meta"] {
                let src = gameSourceURL.appendingPathComponent(sub)
                let dst = destTitleURL.appendingPathComponent(sub)

                guard fileManager.fileExists(atPath: src.path) else { continue }

                // Keep real MLC installs intact. Only links created by the
                // library mirror may be refreshed when the Games folder moves.
                var isDir: ObjCBool = false
                let exists = fileManager.fileExists(atPath: dst.path, isDirectory: &isDir)
                if let currentLink = try? fileManager.destinationOfSymbolicLink(atPath: dst.path) {
                    let currentTarget: URL
                    if currentLink.hasPrefix("/") {
                        currentTarget = URL(fileURLWithPath: currentLink)
                    } else {
                        currentTarget = dst.deletingLastPathComponent().appendingPathComponent(currentLink)
                    }
                    if exists && currentTarget.standardizedFileURL == src.standardizedFileURL {
                        continue
                    }
                    try? fileManager.removeItem(at: dst)
                } else if exists {
                    continue
                }

                if src.path.hasPrefix(documentsURL.path + "/") {
                    let relFromDocs = String(src.path.dropFirst(documentsURL.path.count)).trimmingCharacters(in: CharacterSet(charactersIn: "/"))
                    let relativeDestination = "../../../../../" + relFromDocs
                    try? fileManager.createSymbolicLink(atPath: dst.path, withDestinationPath: relativeDestination)
                } else {
                    try? fileManager.createSymbolicLink(at: dst, withDestinationURL: src)
                }
                if !fileManager.fileExists(atPath: dst.path) {
                    try? fileManager.copyItem(at: src, to: dst)
                }
            }
        }
    }

    private static func isExtractedTitle(_ url: URL, fileManager: FileManager) -> Bool {
        let metaXML = url.appendingPathComponent("meta/meta.xml")
        let codeDirectory = url.appendingPathComponent("code", isDirectory: true)
        guard fileManager.fileExists(atPath: metaXML.path),
              let codeFiles = try? fileManager.contentsOfDirectory(at: codeDirectory, includingPropertiesForKeys: nil) else {
            return false
        }
        return codeFiles.contains { $0.pathExtension.lowercased() == "rpx" }
    }

    private static func isSupportedGameFile(_ url: URL, fileManager: FileManager) -> Bool {
        var isDirectory: ObjCBool = false
        guard fileManager.fileExists(atPath: url.path, isDirectory: &isDirectory), !isDirectory.boolValue else {
            return false
        }
        return ["wua", "wud", "wux"].contains(url.pathExtension.lowercased())
    }

    private static func regionName(_ rawValue: UInt32) -> String? {
        switch rawValue {
        case 0x1: return "Japan"
        case 0x2: return "USA"
        case 0x4: return "Europe"
        case 0x10: return "China"
        case 0x20: return "Korea"
        case 0x40: return "Taiwan"
        default: return nil
        }
    }

    private static func displayTitle(for url: URL) -> String {
        let rawName = url.lastPathComponent.replacingOccurrences(of: "_", with: " ")
        let withoutTags = rawName.split(separator: "[", maxSplits: 1).first.map(String.init) ?? rawName
        let trimmed = withoutTags.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? "Wii U Game" : trimmed
    }
}

private struct ArtworkEnhancementStatusView: View {
    let progress: ArtworkEnhancementProgress
    var body: some View {
        HStack(spacing: 10) {
            if progress.isPaused { Image(systemName: "pause.circle") }
            else { ProgressView().controlSize(.small) }
            VStack(alignment: .leading, spacing: 2) {
                Text(progress.isPaused ? "Cover enhancement paused" : "Enhancing covers")
                    .font(.caption.weight(.medium))
                Text(progress.isPaused ? "Resumes when the device cools" : "\(progress.completed) of \(progress.total)")
                    .font(.caption2).foregroundStyle(.secondary)
            }
        }
        .padding(12)
        .modifier(LibraryGlassSurface(cornerRadius: 20))
        .accessibilityElement(children: .combine)
    }
}

final class DocumentPickerPresenter: NSObject, UIDocumentPickerDelegate {
    static let shared = DocumentPickerPresenter()

    private var completion: ((Result<URL, Error>) -> Void)?
    private var activePicker: UIDocumentPickerViewController?

    func present(
        allowedTypes: [UTType],
        completion: @escaping (Result<URL, Error>) -> Void
    ) {
        self.completion = completion
        if Thread.isMainThread {
            self.schedulePresentation(allowedTypes: allowedTypes, attempt: 0)
        } else {
            DispatchQueue.main.async {
                self.schedulePresentation(allowedTypes: allowedTypes, attempt: 0)
            }
        }
    }

    private func schedulePresentation(allowedTypes: [UTType], attempt: Int) {
        guard attempt < 30 else {
            let error = CocoaError(.coderInvalidValue)
            let cb = completion
            completion = nil
            cb?(.failure(error))
            return
        }

        guard let topVC = Self.topViewController() else {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                self.schedulePresentation(allowedTypes: allowedTypes, attempt: attempt + 1)
            }
            return
        }

        // If topVC is transitioning or a presentedViewController is active / dismissing (e.g. a UIMenu):
        if topVC.isBeingDismissed || topVC.isBeingPresented || topVC.transitionCoordinator != nil || topVC.presentedViewController != nil {
            let coordinator = topVC.transitionCoordinator ?? topVC.presentedViewController?.transitionCoordinator
            if let coordinator = coordinator {
                coordinator.animate(alongsideTransition: nil) { [weak self] _ in
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
                        self?.schedulePresentation(allowedTypes: allowedTypes, attempt: attempt + 1)
                    }
                }
            } else {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                    self.schedulePresentation(allowedTypes: allowedTypes, attempt: attempt + 1)
                }
            }
            return
        }

        let picker = UIDocumentPickerViewController(forOpeningContentTypes: allowedTypes, asCopy: false)
        picker.delegate = self
        picker.allowsMultipleSelection = false
        picker.modalPresentationStyle = .formSheet
        self.activePicker = picker

        topVC.present(picker, animated: true)
    }

    private static func topViewController(base: UIViewController? = nil) -> UIViewController? {
        let root: UIViewController?
        if let base = base {
            root = base
        } else {
            let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
            let activeScene = scenes.first(where: { $0.activationState == .foregroundActive }) ?? scenes.first
            root = activeScene?.windows.first(where: { $0.isKeyWindow })?.rootViewController ?? activeScene?.windows.first?.rootViewController
        }

        guard let root = root else { return nil }

        if let nav = root as? UINavigationController {
            return topViewController(base: nav.visibleViewController ?? nav.topViewController)
        }
        if let tab = root as? UITabBarController {
            return topViewController(base: tab.selectedViewController)
        }
        if let presented = root.presentedViewController, !presented.isBeingDismissed {
            return topViewController(base: presented)
        }
        return root
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        activePicker = nil
        guard let url = urls.first else {
            let cb = completion
            completion = nil
            cb?(.failure(CocoaError(.fileNoSuchFile)))
            return
        }
        let cb = completion
        completion = nil
        cb?(.success(url))
    }

    func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
        activePicker = nil
        let cb = completion
        completion = nil
        cb?(.failure(CocoaError(.userCancelled)))
    }
}

private struct GameLibraryView: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.colorScheme) private var colorScheme
    @State private var isGuidePresented = !WelcomeProgress.isComplete()
    @State private var guideIsFirstRun = !WelcomeProgress.isComplete()
    @ObservedObject var library: GameLibraryStore
    @ObservedObject private var theme = MenuThemeStore.shared
    @State private var selectedSaveGame: GameLibraryEntry?
    @State private var selectedGraphicsGame: GameLibraryEntry?
    @State private var isSettingsPresented = false
    @State private var installImporterError: String?
    @State private var showLandscapeWarning = false
    @AppStorage("CemuIOS.SelectedLibraryGame") private var selectedBackgroundGame = ""
    @AppStorage("CemuIOS.LibraryStyle") private var libraryStyle = "covers"
    let chooseFolder: () -> Void
    let installTitleContent: (URL) -> Void
    let installSystemMenu: (URL) -> Void
    let showDeveloperTools: () -> Void
    let launch: (GameLibraryEntry) async -> Bool

    private var isPhone: Bool {
        UIDevice.current.userInterfaceIdiom == .phone
    }

    private var emptyLibrary: some View {
        ContentUnavailableView {
            Label("Your library", systemImage: "rectangle.stack")
        } description: {
            Text("Add an extracted Wii U game folder or WUA file.")
        } actions: {
            Button("Add games", systemImage: "plus", action: chooseFolder)
                .modifier(NativeGlassButton(prominent: true))
                .accessibilityIdentifier("library.addGame")
            Button("Open 77U Games folder") { library.useAppGamesFolder(asDefault: false) }
                .modifier(NativeGlassButton())
            Text(library.statusMessage).font(.footnote).foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
        .modifier(LibraryEntrance(delay: 0.1))
    }

    var body: some View {
        return GeometryReader { proxy in
            let phone = UIDevice.current.userInterfaceIdiom == .phone
            let sideInset: CGFloat = phone ? 16 : 32

            ZStack {
                let selected = library.entries.first(where: { $0.id == selectedBackgroundGame }) ?? library.entries.first
                let selectedURL = selected.flatMap { library.url(for: $0) }
                GameLibraryBackdrop(titleURL: selectedURL, isGameFile: selected?.isGameFile ?? false)
                    .onChange(of: selectedURL, initial: true) { _, url in
                        theme.selectGame(url, isGameFile: selected?.isGameFile ?? false)
                    }

                VStack(alignment: .leading, spacing: 0) {
                    LibraryHeader(
                        count: library.entries.count,
                        settings: { isSettingsPresented = true },
                        defaultFolder: {
                            if library.hasDefaultGamesFolder {
                                library.openDefaultGamesFolder()
                                installImporterError = library.defaultFolderError
                            } else {
                                chooseDefaultFolder()
                            }
                        },
                        chooseDefaultFolder: chooseDefaultFolder,
                        appFolder: {
                            library.useAppGamesFolder(asDefault: false)
                            installImporterError = library.defaultFolderError
                        },
                        chooseFolder: chooseFolder,
                        installContent: {
                            DocumentPickerPresenter.shared.present(allowedTypes: [.folder, .item]) { result in
                                switch result {
                                case .success(let url):
                                    installTitleContent(url)
                                case .failure(let error):
                                    let nsError = error as NSError
                                    if nsError.code != NSUserCancelledError && nsError.code != CocoaError.userCancelled.rawValue {
                                        installImporterError = error.localizedDescription
                                    }
                                }
                            }
                        },
                        installSystem: {
                            DocumentPickerPresenter.shared.present(allowedTypes: [.folder, .item]) { result in
                                switch result {
                                case .success(let url):
                                    installSystemMenu(url)
                                case .failure(let error):
                                    let nsError = error as NSError
                                    if nsError.code != NSUserCancelledError && nsError.code != CocoaError.userCancelled.rawValue {
                                        installImporterError = error.localizedDescription
                                    }
                                }
                            }
                        }
                    )
                    .padding(.horizontal, sideInset)
                    .padding(.top, phone ? 8 : 16)
                    .padding(.bottom, 12)
                    .anchorPreference(key: LibraryHeaderBoundsKey.self, value: .bounds) { $0 }

                    if library.entries.isEmpty {
                        if phone {
                            ScrollView { emptyLibrary }
                        } else {
                            emptyLibrary
                        }
                    } else {
                        LibraryLayoutTransition(style: libraryStyle) {
                            GameCoverFlowView(games: library.entries,
                                play: { await attemptLaunch($0, in: proxy.size) },
                                manageSaves: { selectedSaveGame = $0 },
                                manageGraphics: { selectedGraphicsGame = $0 },
                                isEnhancingArtwork: library.artworkProgress != nil)
                        } grid: {
                            GameLibraryGridView(games: library.entries,
                                play: { await attemptLaunch($0, in: proxy.size) },
                                manageSaves: { selectedSaveGame = $0 },
                                manageGraphics: { selectedGraphicsGame = $0 },
                                isEnhancingArtwork: library.artworkProgress != nil)
                        }
                    }
                }
                .frame(width: proxy.size.width, height: proxy.size.height, alignment: .topLeading)


            }
            .frame(width: proxy.size.width, height: proxy.size.height)
            .modifier(LibraryNoticeOverlay(horizontalInset: sideInset) {
                Group {
                    if let progress = library.artworkProgress {
                        ArtworkEnhancementStatusView(progress: progress)
                            .transition(reduceMotion ? .opacity : .opacity.combined(with: .move(edge: .top)))
                    }
                }
                .animation(reduceMotion ? nil : .smooth(duration: 0.25), value: library.artworkProgress != nil)
            })
        }
        .foregroundStyle(theme.configuration.libraryInk(for: colorScheme))
        .onAppear { MenuThemeStore.shared.setMenuVisible(!GameOrientationCoordinator.isGameActive) }
        .animation(reduceMotion ? nil : .easeInOut(duration: 0.35), value: colorScheme)
        .fullScreenCover(isPresented: $isGuidePresented) {
            WelcomeGuideView(isFirstRun: guideIsFirstRun) { isGuidePresented = false }
        }
        .sheet(item: $selectedSaveGame) { game in
            SaveManagementView(game: game)
        }
        .sheet(item: $selectedGraphicsGame) { game in
            GraphicsPacksView(game: game)
        }
        .sheet(isPresented: $isSettingsPresented) {
            AppSettingsView(showDeveloperTools: showDeveloperTools, systemColorScheme: colorScheme, library: library)
        }
        .alert("Couldn't open files", isPresented: Binding(
            get: { installImporterError != nil },
            set: { if !$0 { installImporterError = nil } }
        )) {
            Button("OK", role: .cancel) { installImporterError = nil }
        } message: {
            Text(installImporterError ?? "")
        }
        .alert("Rotate to landscape", isPresented: $showLandscapeWarning) {
            Button("OK", role: .cancel) {}
        } message: {
            Text("Turn your iPad sideways, then tap Play.")
        }
        .onReceive(NotificationCenter.default.publisher(for: Notification.Name("NSProcessInfoThermalStateDidChangeNotification"))) { _ in
            library.resumeArtworkEnhancementIfNeeded()
        }
        .onReceive(NotificationCenter.default.publisher(for: Notification.Name("NSProcessInfoPowerStateDidChangeNotification"))) { _ in
            library.resumeArtworkEnhancementIfNeeded()
        }
        .onReceive(NotificationCenter.default.publisher(for: GameArtworkEnhancer.preferenceDidChange)) { _ in
            library.applyArtworkEnhancementPreference()
        }
    }

    private func chooseDefaultFolder() {
        DocumentPickerPresenter.shared.present(allowedTypes: [.folder]) { result in
            do { try library.setDefaultGamesFolder(result.get()) }
            catch CocoaError.userCancelled {}
            catch { installImporterError = error.localizedDescription }
        }
    }

    private func attemptLaunch(_ game: GameLibraryEntry, in size: CGSize) async -> Bool {
        if UIDevice.current.userInterfaceIdiom != .phone && size.width <= size.height {
            showLandscapeWarning = true
            return false
        }
        return await launch(game)
    }
}

struct ContentView: View {
    @Environment(\.colorScheme) private var colorScheme
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    @StateObject private var playPreferences = ExternalDisplayCoordinator.shared.preferences
    @StateObject private var touchInput = TouchInputSession()
    @StateObject private var vm = BridgeViewModel()
    @StateObject private var gameLibrary = GameLibraryStore()
    @State private var launchingGame: GameLibraryEntry?
    @State private var lastAttemptedGame: GameLibraryEntry?
    @State private var copiedFeedback: Bool = false
    @State private var showDiagnostics: Bool = false
    @State private var isPortraitWindow = false
    @AppStorage("CemuIOS.ShowFPSOverlay") private var showFPSOverlay = true
    @AppStorage("CemuIOS.ShowDRCPreview") private var showDRCPreview = false
    @AppStorage("CemuIOS.ShowDebugButton") private var showDebugButton = false
    @State private var selectedCPUMode: Int = Int(CemuBridge_GetCPUDiagnosticMode())
    @State private var selectedTexCacheMode: Int = Int(CemuBridge_GetTextureCacheDiagMode())
    @State private var selectedRTStoreMode: Int = Int(CemuBridge_GetRTStoreDiagMode())
    @State private var isInstallingTitleContent = false
    @State private var isInstallingSystemMenu = false
    @State private var titleContentInstallMessage: String?
    @State private var systemMenuInstallMessage: String?
    @State private var diagRefreshCounter = 0
    @State private var showLowMemoryWarning = false
    @State private var isExitingGame = false
    @State private var pendingLaunchAfterWarning: GameLibraryEntry?
    private var bridgeInfo: CemuBridgeInfo {
        vm.bridgeInfo
    }

    private var jitLogInfo: (exists: Bool, lines: [String]) {
        guard showDiagnostics else { return (false, []) }
        var jitBuffer = [CChar](repeating: 0, count: 4096)
        CemuBridge_GetJITDiagnosticsRecent(&jitBuffer, 4096, 20)
        let jitContent = stringFromNullTerminatedBuffer(jitBuffer)
        let jitLines = jitContent.components(separatedBy: "\n").filter { !$0.isEmpty }
        return (exists: !jitLines.isEmpty, lines: jitLines)
    }

    private let updateTimer = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    var body: some View {
        ZStack {
            // Keep the renderer surfaces attached before launch. The library is
            // an opaque app-layer cover and does not alter renderer lifecycle.
            MetalDisplayStack(onLayerAttached: { attached in
                // Refresh bridge info immediately when the layer is attached to the window
                vm.refresh()
            }, showsDRCPreview: showDRCPreview, preferences: playPreferences)
            .ignoresSafeArea()

            ZStack {
                if !bridgeInfo.titleBootResult.titleLoaded {
                    GameLibraryView(
                        library: gameLibrary,
                        chooseFolder: {
                            DocumentPickerPresenter.shared.present(allowedTypes: wiiUGameLibraryContentTypes) { result in
                                switch result {
                                case .success(let selectedURL):
                                    gameLibrary.selectLibraryLocation(selectedURL)
                                case .failure(let error):
                                    let nsError = error as NSError
                                    if nsError.code != NSUserCancelledError && nsError.code != CocoaError.userCancelled.rawValue {
                                        CemuBridge_LogJITMilestone("[LIBRARY] Folder selection failed: \(error.localizedDescription)")
                                    }
                                }
                            }
                        },
                        installTitleContent: { installTitleContent(from: $0) },
                        installSystemMenu: { installSystemFirmware(from: $0) },
                        showDeveloperTools: { showDiagnostics = true },
                        launch: { await launchGame($0) }
                    )
                    .zIndex(1)
                }
            }
            .allowsHitTesting(!bridgeInfo.titleBootResult.titleLoaded)

            // Overlay with bridge status (scrollable validation display)
            if showDiagnostics {
                ScrollView {
                    VStack(alignment: .leading, spacing: 4) {
                        DiagnosticsActionsView(open: { loadTitleButton }, copy: { copyDiagnosticsButton },
                                               save: { saveFullDiagnosticsButton }) {
                            withAnimation(.easeInOut(duration: 0.2)) { showDiagnostics = false }
                        }

                        if bridgeInfo.titleBootResult.titleLoaded && CemuBridge_IsGPUFrameCaptureSupported() {
                            HStack(spacing: 8) {
                                Button("Capture Graphics Frame", systemImage: "camera.viewfinder") {
                                    if CemuBridge_RequestGPUFrameCapture() {
                                        showDiagnostics = false
                                    }
                                }
                                .controllerButtonStyle(.bordered)
                                Text("One frame saved in GPUCaptures. Capture may briefly pause play.")
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            }
                        }

                    VStack(alignment: .leading, spacing: 6) {
                        VStack(alignment: .leading, spacing: 8) {
                            Text("CPU mode")
                                .font(.caption)
                                .bold()
                            Picker("CPU Mode", selection: $selectedCPUMode) {
                                Text("Single-core").tag(0)
                                Text("Multi-core").tag(2)
                                Text("Interpreter").tag(1)
                            }
                            .pickerStyle(.segmented)
                            .disabled(bridgeInfo.titleBootResult.titleLoaded)
                            .onChange(of: selectedCPUMode) { _, newMode in
                                CemuBridge_SetCPUDiagnosticMode(Int32(newMode))
                                vm.refresh()
                            }

                            Toggle(isOn: $showFPSOverlay) {
                                Text("Show FPS and game speed")
                                    .font(.caption)
                            }.controllerToggle($showFPSOverlay)
                        }

                        DisclosureGroup("Renderer troubleshooting") {
                        VStack(alignment: .leading, spacing: 8) {
                            Text("Texture cache")
                                .font(.caption)
                                .bold()
                            Picker("Texture Cache", selection: $selectedTexCacheMode) {
                                Text("Normal").tag(0)
                                Text("Check 1280 Width").tag(1)
                                Text("Full Hash Diag").tag(2)
                                Text("Force Refresh (Slow)").tag(3)
                            }
                            .pickerStyle(.menu)
                            .onChange(of: selectedTexCacheMode) { _, newMode in
                                CemuBridge_SetTextureCacheDiagMode(Int32(newMode))
                                vm.refresh()
                            }

                            Text("Render target storage")
                                .font(.caption)
                                .bold()
                            Picker("RT Store", selection: $selectedRTStoreMode) {
                                Text("Normal").tag(0)
                                Text("Force Store Color Attachments").tag(1)
                            }
                            .pickerStyle(.menu)
                            .onChange(of: selectedRTStoreMode) { _, newMode in
                                CemuBridge_SetRTStoreDiagMode(Int32(newMode))
                                vm.refresh()
                            }
                        }
                        Text("For troubleshooting only. Non-default modes can reduce performance.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        }
                    }
                    .padding(.vertical, 4)

                    Divider()
                    HStack {
                        Text("JIT Diagnostics")
                            .font(.caption)
                            .bold()
                        Spacer()
                        Button(action: {
                            CemuBridge_ClearJITDiagnosticsLog()
                            vm.refresh()
                        }) {
                            Text("Clear JIT Log")
                                .font(.system(size: 10, weight: .semibold))
                                .foregroundColor(.white)
                                .padding(.horizontal, 6)
                                .padding(.vertical, 3)
                                .background(Color.red.opacity(0.7))
                                .cornerRadius(4)
                        }
                    }
                    let info = jitLogInfo
                    Text("Log Exists: \(info.exists ? "Yes" : "No")")
                        .font(.caption2)
                        .foregroundColor(info.exists ? .green : .secondary)
                    Text("Recent Diagnostic Logs (\(info.lines.count)):")
                        .font(.caption2)
                        .bold()
                    if info.lines.isEmpty {
                        Text("  (None yet)")
                            .font(.caption2)
                            .foregroundColor(.secondary)
                    } else {
                        ForEach(info.lines, id: \.self) { line in
                            Text(line)
                                .font(.system(size: 10, design: .monospaced))
                                .foregroundColor(line.contains("FAILED") ? .red : (line.contains("SESSION START") ? .yellow : (line.contains("entered") || line.contains("succeeded") ? .green : .primary)))
                        }
                    }

                    Divider()
                    if bridgeInfo.metalAvailable {
                        let deviceName = bridgeInfo.metalDeviceName.map { String(cString: $0) } ?? "Unknown"
                        Text("Metal Device: \(deviceName)")
                            .font(.caption)
                        Text("Layer Attached: \(bridgeInfo.metalLayerAttached ? "Yes" : "No")")
                            .font(.caption)
                            .foregroundColor(bridgeInfo.metalLayerAttached ? .green : .red)
                    } else {
                        Text("Metal: Unavailable")
                            .font(.caption)
                            .foregroundColor(.red)
                    }

                    if let version = bridgeInfo.cemuVersion {
                        Text("Core Version: \(String(cString: version))")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }

                    if !bridgeInfo.titleBootResult.titleLoaded {
                        Text(bridgeInfo.metalLayerAttached ? "Metal Renderer: Ready to start a game" : "Metal Renderer: Waiting for display")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                }
                .padding(12)
                .background(Color(uiColor: .systemBackground), in: RoundedRectangle(cornerRadius: 10))
                .padding(.horizontal, 16)
                .padding(.vertical, 24)
            }
        }
        if !showDiagnostics && bridgeInfo.titleBootResult.titleLoaded {
            PlayOverlay(preferences: playPreferences, input: touchInput,
                        showFPS: $showFPSOverlay, showGamePad: $showDRCPreview,
                        showDebugButton: $showDebugButton,
                        showDiagnostics: { showDiagnostics = true },
                        onShareDiagnostics: { exportFullDiagnostics() },
                        onExitGame: {
                            guard !isExitingGame else { return }
                            withAnimation(.easeInOut(duration: reduceMotion ? 0.12 : 0.32)) {
                                isExitingGame = true
                            }
                            Task { @MainActor in
                                try? await Task.sleep(nanoseconds: 280_000_000)
                                vm.stopCurrentGame()
                                gameLibrary.resumeArtworkEnhancementAfterGame()
                                try? await Task.sleep(nanoseconds: 60_000_000)
                                withAnimation(.easeOut(duration: reduceMotion ? 0.15 : 0.35)) {
                                    isExitingGame = false
                                }
                            }
                        })
        }
    }
        .animation(reduceMotion ? nil : .easeInOut(duration: 0.35), value: colorScheme)
        .overlay {
            if isExitingGame {
                ZStack {
                    Rectangle()
                        .fill(.ultraThinMaterial)
                    Color.black.opacity(0.3)
                }
                .ignoresSafeArea()
                .transition(.opacity)
                .zIndex(98)
                .allowsHitTesting(true)
            }
        }
        .animation(.easeInOut(duration: reduceMotion ? 0.15 : 0.32), value: isExitingGame)
        .overlay {
            ZStack {
                if let game = launchingGame {
                    LibraryLaunchScreen(game: game)
                        .transition(.opacity)
                        .zIndex(1)
                }
            }
            .animation(.easeInOut(duration: reduceMotion ? 0.12 : 0.32), value: launchingGame?.id)
        }
        .overlay {
            if UIDevice.current.userInterfaceIdiom == .pad && isPortraitWindow && (bridgeInfo.titleBootResult.titleLoaded || vm.isLaunching) {
                ZStack {
                    Color.black.ignoresSafeArea()
                    VStack(spacing: 14) {
                        Image(systemName: "ipad.landscape")
                            .font(.system(size: 42))
                        Text("Rotate iPad to Landscape")
                            .font(.title2.bold())
                        Text("The game is available only in landscape. Return to a landscape-sized window to continue.")
                            .multilineTextAlignment(.center)
                            .foregroundStyle(.secondary)
                    }
                    .padding(32)
                }
            }
        }
        .onGeometryChange(for: Bool.self, of: { geometry in
            geometry.size.height > geometry.size.width
        }) { portrait in
            isPortraitWindow = portrait
            GameOrientationCoordinator.setPortraitBlocked(UIDevice.current.userInterfaceIdiom == .pad && portrait)
        }
        .onChange(of: bridgeInfo.titleBootResult.titleLoaded) { _, loaded in
            MenuThemeStore.shared.setGameRunning(loaded)
            GameOrientationCoordinator.setGameActive(loaded || vm.isLaunching)
            if loaded { launchingGame = nil }
            else if !vm.isLaunching { gameLibrary.resumeArtworkEnhancementAfterGame() }
        }
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.didReceiveMemoryWarningNotification)) { _ in
            gameLibrary.handleArtworkMemoryWarning()
        }
        .disabled(vm.isLaunching || launchingGame != nil)
        .alert("Could not start game", isPresented: Binding(
            get: { vm.launchError != nil }, set: { if !$0 { vm.launchError = nil } }
        )) {
            if let fallbackGame = lastAttemptedGame {
                Button("Try again") {
                    let gameToLaunch = fallbackGame
                    vm.launchError = nil
                    Task {
                        await launchGame(gameToLaunch)
                    }
                }
            }
            if JITCoordinator.shared.inLiveContainer, let fallbackGame = lastAttemptedGame {
                Button("Launch without JIT") {
                    vm.launchError = nil
                    CemuBridge_SetCPUDiagnosticMode(1)
                    Task {
                        await launchGame(fallbackGame)
                    }
                }
            }
            Button("Cancel", role: .cancel) { vm.launchError = nil }
        } message: { Text(vm.launchError ?? "") }
        .environmentObject(playPreferences)
        .onAppear {
            let previousCrashPath = String(cString: CemuBridge_GetPreviousCrashReportPath())
            if !previousCrashPath.isEmpty && FileManager.default.fileExists(atPath: previousCrashPath) {
                UserDefaults.standard.removeObject(forKey: "77U.pendingLaunchGameID")
            }
            vm.refresh()
            gameLibrary.restoreLibraryFolder()
            ExternalDisplayCoordinator.shared.isGameRunning = vm.bridgeInfo.titleBootResult.titleLoaded
        }
        .onOpenURL { url in
            if url.scheme == "cemu77u-jit" {
                JITCoordinator.shared.handleVPNReturn(url)
                Task { @MainActor in
                    try? await Task.sleep(for: .milliseconds(350))
                    resumePendingLaunchIfNeeded()
                }
                return
            }
            guard ["wua", "wud", "wux"].contains(url.pathExtension.lowercased()) else { return }
            gameLibrary.selectLibraryLocation(url)
        }
        .onReceive(updateTimer) { _ in
            guard !vm.isLaunching, !vm.isPreparingJIT else { return }
            if CemuBridge_IsTitleRunning() {
                guard showDiagnostics else { return }
                diagRefreshCounter += 1
                if diagRefreshCounter % 3 != 0 { return }
            }
            vm.refresh()
            let isGameRunning = vm.bridgeInfo.titleBootResult.titleLoaded
            if ExternalDisplayCoordinator.shared.isGameRunning != isGameRunning {
                ExternalDisplayCoordinator.shared.isGameRunning = isGameRunning
            }
        }
        .alert("Update & DLC", isPresented: Binding(
            get: { titleContentInstallMessage != nil },
            set: { if !$0 { titleContentInstallMessage = nil } }
        )) {
            Button("OK", role: .cancel) { titleContentInstallMessage = nil }
        } message: {
            Text(titleContentInstallMessage ?? "")
        }
        .alert("Wii U System Menu", isPresented: Binding(
            get: { systemMenuInstallMessage != nil },
            set: { if !$0 { systemMenuInstallMessage = nil } }
        )) {
            Button("OK", role: .cancel) { systemMenuInstallMessage = nil }
        } message: {
            Text(systemMenuInstallMessage ?? "")
        }
        .alert("JIT Required", isPresented: $vm.isJITWarningPresented) {
            Button("OK", role: .cancel) {}
        } message: {
            Text("Enable JIT with StikDebug, then try launching the game again.")
        }
        .alert("Memory allowance", isPresented: $showLowMemoryWarning) {
            Button("Continue") {
                UserDefaults.standard.set(true, forKey: "CemuIOS.DismissedLowMemoryWarning")
                if let game = pendingLaunchAfterWarning {
                    pendingLaunchAfterWarning = nil
                    Task { await executeLaunch(game) }
                }
            }
            Button("Cancel", role: .cancel) {
                pendingLaunchAfterWarning = nil
            }
        } message: {
            Text("Games may run out of memory on this device. GetMoreRAM is optional but recommended: it can enable a higher app memory limit on supported devices and installations. Setup is available in Settings -> Welcome and imports.")
        }
        .overlay {
            if isInstallingTitleContent || isInstallingSystemMenu {
                ZStack {
                    Color.black.opacity(0.45).ignoresSafeArea()
                    ProgressView(isInstallingSystemMenu ? "Installing Wii U Menu & System Files…" : "Installing update or DLC…")
                        .padding(24)
                        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 18))
                }
            }
        }
    }

    private func installSystemFirmware(from selectedURL: URL) {
        let hasSecurityScope = selectedURL.startAccessingSecurityScopedResource()
        isInstallingSystemMenu = true
        DispatchQueue.global(qos: .userInitiated).async {
            let fileManager = FileManager.default
            guard let documentsURL = fileManager.urls(for: .documentDirectory, in: .userDomainMask).first else {
                DispatchQueue.main.async {
                    if hasSecurityScope { selectedURL.stopAccessingSecurityScopedResource() }
                    isInstallingSystemMenu = false
                    systemMenuInstallMessage = "Could not locate app Documents directory."
                }
                return
            }

            var installedAny = false

            func copyFolderRecursive(from src: URL, to dst: URL) {
                try? fileManager.createDirectory(at: dst, withIntermediateDirectories: true)
                guard let items = try? fileManager.contentsOfDirectory(at: src, includingPropertiesForKeys: [.isDirectoryKey]) else { return }
                for item in items {
                    let isDir = (try? item.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true
                    let target = dst.appendingPathComponent(item.lastPathComponent, isDirectory: isDir)
                    if isDir {
                        copyFolderRecursive(from: item, to: target)
                    } else {
                        if fileManager.fileExists(atPath: target.path) {
                            try? fileManager.removeItem(at: target)
                        }
                        try? fileManager.copyItem(at: item, to: target)
                    }
                }
            }

            func scanAndCopy(source: URL) {
                let lowerName = source.lastPathComponent.lowercased()
                if lowerName == "cafelibs" {
                    let dst = documentsURL.appendingPathComponent("cafeLibs", isDirectory: true)
                    copyFolderRecursive(from: source, to: dst)
                    installedAny = true
                    return
                }
                if lowerName == "mlc01" {
                    let dst = documentsURL.appendingPathComponent("mlc01", isDirectory: true)
                    copyFolderRecursive(from: source, to: dst)
                    installedAny = true
                    return
                }
                if lowerName == "sys" && source.deletingLastPathComponent().lastPathComponent.lowercased() == "mlc01" {
                    let dst = documentsURL.appendingPathComponent("mlc01/sys", isDirectory: true)
                    copyFolderRecursive(from: source, to: dst)
                    installedAny = true
                    return
                }

                guard let children = try? fileManager.contentsOfDirectory(at: source, includingPropertiesForKeys: [.isDirectoryKey]) else { return }
                for child in children {
                    if (try? child.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true {
                        scanAndCopy(source: child)
                    }
                }
            }

            scanAndCopy(source: selectedURL)

            DispatchQueue.main.async {
                if hasSecurityScope { selectedURL.stopAccessingSecurityScopedResource() }
                isInstallingSystemMenu = false
                if installedAny {
                    systemMenuInstallMessage = "Wii U Menu & System Libraries installed successfully!"
                    gameLibrary.rescan()
                } else {
                    systemMenuInstallMessage = "No 'mlc01' or 'cafeLibs' folders found in the selected location."
                }
            }
        }
    }

    private func installTitleContent(from selectedURL: URL) {
        let targetURL: URL
        let lowerName = selectedURL.lastPathComponent.lowercased()
        if lowerName == "meta.xml" {
            targetURL = selectedURL.deletingLastPathComponent().deletingLastPathComponent()
        } else if lowerName == "meta" || lowerName == "code" || lowerName == "content" {
            targetURL = selectedURL.deletingLastPathComponent()
        } else {
            targetURL = selectedURL
        }
        let hasSecurityScope = targetURL.startAccessingSecurityScopedResource()
        isInstallingTitleContent = true
        DispatchQueue.global(qos: .userInitiated).async {
            let installResult = targetURL.path.withCString { sourcePath in
                CemuBridge_InstallTitleContent(sourcePath)
            }
            DispatchQueue.main.async {
                if hasSecurityScope {
                    targetURL.stopAccessingSecurityScopedResource()
                }
                isInstallingTitleContent = false
                if installResult.success {
                    let kind = installResult.contentType == 1 ? "Update" : "DLC"
                    titleContentInstallMessage = "\(kind) v\(installResult.titleVersion) installed successfully."
                    gameLibrary.rescan()
                } else {
                    let message = cString(installResult.errorMessage)
                    titleContentInstallMessage = message.isEmpty ? "The update could not be installed." : message
                }
            }
        }
    }

    private var loadTitleButton: some View {
        Button(action: {
            CemuBridge_LogJITMilestone("[JIT] Load Title / RPX button tapped")
            DocumentPickerPresenter.shared.present(allowedTypes: [.folder, .item]) { result in
                switch result {
                case .success(let selectedUrl):
                    CemuBridge_LogJITMilestone("[JIT] File importer selected URL: \(selectedUrl.lastPathComponent)")
                    Task { @MainActor in
                        if !(await vm.launchTitle(from: selectedUrl, prepareArtwork: { await gameLibrary.prepareForGameLaunch() })),
                           !vm.isLaunching, !vm.bridgeInfo.titleBootResult.titleLoaded {
                            gameLibrary.resumeArtworkEnhancementAfterGame()
                        }
                    }
                case .failure(let error):
                    let nsError = error as NSError
                    if nsError.code != NSUserCancelledError && nsError.code != CocoaError.userCancelled.rawValue {
                        CemuBridge_LogJITMilestone("[JIT] File import failed: \(error.localizedDescription)")
                    }
                }
            }
        }) {
            HStack(spacing: 4) {
                Image(systemName: "folder.badge.plus")
                    .font(.system(size: 11, weight: .bold))
                Text("Open game")
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(1)
            }
            .foregroundColor(.white)
            .padding(.horizontal, 10)
            .frame(maxWidth: .infinity, minHeight: 44)
            .background(Color.orange)
            .cornerRadius(6)
        }
    }

    private var copyDiagnosticsButton: some View {
        Button(action: {
            let report = generateDiagnosticsReport()
            UIPasteboard.general.string = report
            withAnimation(.easeInOut(duration: 0.2)) {
                copiedFeedback = true
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) {
                withAnimation(.easeInOut(duration: 0.2)) {
                    copiedFeedback = false
                }
            }
        }) {
            HStack(spacing: 4) {
                Image(systemName: copiedFeedback ? "checkmark.circle.fill" : "doc.on.doc")
                    .font(.system(size: 11, weight: .bold))
                Text(copiedFeedback ? "Copied" : "Copy report")
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(1)
            }
            .foregroundColor(.white)
            .padding(.horizontal, 10)
            .frame(maxWidth: .infinity, minHeight: 44)
            .background(copiedFeedback ? Color.green : Color.blue)
            .cornerRadius(6)
        }
    }

    private func launchGame(_ game: GameLibraryEntry) async -> Bool {
        let memLimit = CemuBridge_GetMemoryLimitInfo()
        let ramGB = Double(memLimit.physicalMemoryBytes) / (1024.0 * 1024.0 * 1024.0)
        let hasDismissed = UserDefaults.standard.bool(forKey: "CemuIOS.DismissedLowMemoryWarning")

        if (ramGB < 5.8 || (ramGB <= 6.5 && !memLimit.increasedMemoryLimitActive)) && !hasDismissed {
            pendingLaunchAfterWarning = game
            showLowMemoryWarning = true
            return false
        }
        return await executeLaunch(game)
    }

    private func executeLaunch(_ game: GameLibraryEntry) async -> Bool {
        guard launchingGame == nil, !vm.isLaunching, !vm.bridgeInfo.titleBootResult.titleLoaded,
              let url = gameLibrary.url(for: game) else { return false }
        launchingGame = game
        lastAttemptedGame = game
        do {
            // Finish presenting the launch screen before native title preparation begins.
            if !reduceMotion { try await Task.sleep(for: .milliseconds(320)) }
            try Task.checkCancellation()
        } catch {
            launchingGame = nil
            return false
        }
        UserDefaults.standard.set(game.id, forKey: "77U.pendingLaunchGameID")
        GameOrientationCoordinator.setGameActive(true)
        let launched = await vm.launchTitle(from: url, prepareArtwork: { await gameLibrary.prepareForGameLaunch() })
        if launched {
            UserDefaults.standard.removeObject(forKey: "77U.pendingLaunchGameID")
            gameLibrary.markPlayed(game)
        } else if !vm.isLaunching && !vm.bridgeInfo.titleBootResult.titleLoaded {
            launchingGame = nil
            UserDefaults.standard.removeObject(forKey: "77U.pendingLaunchGameID")
            GameOrientationCoordinator.setGameActive(false)
            gameLibrary.resumeArtworkEnhancementAfterGame()
        }
        return launched
    }

    private func resumePendingLaunchIfNeeded() {
        guard !ProcessInfo.processInfo.arguments.contains("--jit-activation-self-test") &&
              !ProcessInfo.processInfo.arguments.contains("--jit-helper-self-test") else { return }
        guard !vm.isLaunching, !vm.bridgeInfo.titleBootResult.titleLoaded else { return }
        guard let savedID = UserDefaults.standard.string(forKey: "77U.pendingLaunchGameID"),
              let game = gameLibrary.entries.first(where: { $0.id == savedID }) else { return }

        // Crash loop protection: clear the pending ID immediately so a crash during launch
        // does not permanently lock the user out of the app upon reopening.
        UserDefaults.standard.removeObject(forKey: "77U.pendingLaunchGameID")
        Task { await launchGame(game) }
    }

    private var saveFullDiagnosticsButton: some View {
        Button(action: {
            exportFullDiagnostics()
        }) {
            HStack(spacing: 4) {
                Image(systemName: "square.and.arrow.up")
                    .font(.system(size: 11, weight: .bold))
                Text("Save logs")
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(1)
            }
            .foregroundColor(.white)
            .padding(.horizontal, 10)
            .frame(maxWidth: .infinity, minHeight: 44)
            .background(Color.purple)
            .cornerRadius(6)
        }
    }

    @MainActor
    private func exportFullDiagnostics() {
        let report = generateDiagnosticsReport()
        DispatchQueue.global(qos: .userInitiated).async {
            var exportItems: [Any] = []
            let fileManager = FileManager.default
            let timestamp = Int(Date().timeIntervalSince1970)
            let exportDirectory = fileManager.urls(for: .documentDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("Logs", isDirectory: true)
                .appendingPathComponent("Reports", isDirectory: true)
                .appendingPathComponent("debug-\(timestamp)-\(UUID().uuidString.prefix(8))", isDirectory: true)
            try? fileManager.createDirectory(at: exportDirectory, withIntermediateDirectories: true)

            let diagnosticsURL = exportDirectory.appendingPathComponent("diagnostics.txt")
            if let diagnosticsData = report.data(using: .utf8) {
                try? diagnosticsData.write(to: diagnosticsURL, options: .atomic)
                if fileManager.fileExists(atPath: diagnosticsURL.path) {
                    exportItems.append(diagnosticsURL)
                }
            }

            func appendLogFile(path: String, exportName: String) {
                guard !path.isEmpty, fileManager.fileExists(atPath: path) else { return }
                let destination = exportDirectory.appendingPathComponent(exportName)
                do {
                    try fileManager.copyItem(at: URL(fileURLWithPath: path), to: destination)
                    exportItems.append(destination)
                } catch {
                    // The live logger may rotate between the existence check and
                    // copy. Diagnostics export remains useful without that file.
                }
            }

            appendLogFile(path: String(cString: CemuBridge_GetMainLogPath()), exportName: "log.txt")
            appendLogFile(path: String(cString: CemuBridge_GetJITDiagnosticsPath()), exportName: "jit_diagnostics.log")
            appendLogFile(path: String(cString: CemuBridge_GetPreviousCrashReportPath()), exportName: "last_crash_report.txt")

            let previousLogDirectory = String(cString: CemuBridge_GetPreviousLogDirectory())
            if !previousLogDirectory.isEmpty {
                for name in ["log.txt", "jit_diagnostics.log", "legacy_log.txt", "legacy_jit_diagnostics.log"] {
                    appendLogFile(
                        path: URL(fileURLWithPath: previousLogDirectory).appendingPathComponent(name).path,
                        exportName: "previous_\(name)")
                }
            }

            var performanceBuffer = [CChar](repeating: 0, count: 1_048_576)
            if CemuBridge_GetFramePerformanceReportText(&performanceBuffer, performanceBuffer.count) > 0 {
                let performanceURL = exportDirectory.appendingPathComponent("performance_report.txt")
                let performanceText = stringFromNullTerminatedBuffer(performanceBuffer)
                if let data = performanceText.data(using: .utf8) {
                    try? data.write(to: performanceURL, options: .atomic)
                    if fileManager.fileExists(atPath: performanceURL.path) {
                        exportItems.append(performanceURL)
                    }
                }
            }

            var metalHistoryBuffer = [CChar](repeating: 0, count: 131_072)
            if CemuBridge_GetMetalResourceCrashHistoryText(&metalHistoryBuffer, metalHistoryBuffer.count) > 0 {
                let metalHistoryURL = exportDirectory.appendingPathComponent("metal_resource_history.txt")
                let metalHistory = stringFromNullTerminatedBuffer(metalHistoryBuffer)
                if let metalHistoryData = metalHistory.data(using: .utf8) {
                    try? metalHistoryData.write(to: metalHistoryURL, options: .atomic)
                    if fileManager.fileExists(atPath: metalHistoryURL.path) {
                        exportItems.append(metalHistoryURL)
                    }
                }
            }

            DispatchQueue.main.async {
                UIPasteboard.general.string = report
                if !exportItems.isEmpty {
                    self.presentActivitySheet(items: exportItems)
                }
            }
        }
    }

    private func presentActivitySheet(items: [Any]) {
        guard let windowScene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .first(where: { $0.activationState == .foregroundActive })
            ?? UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene }).first,
            let keyWindow = windowScene.windows.first(where: { $0.isKeyWindow }) ?? windowScene.windows.first,
            let rootVC = keyWindow.rootViewController else {
            return
        }

        var topVC = rootVC
        while let presented = topVC.presentedViewController, !presented.isBeingDismissed {
            topVC = presented
        }

        if topVC is UIActivityViewController {
            return
        }

        let activityVC = UIActivityViewController(activityItems: items, applicationActivities: nil)
        if let popover = activityVC.popoverPresentationController {
            popover.sourceView = topVC.view
            let bounds = topVC.view.bounds
            popover.sourceRect = CGRect(x: bounds.midX, y: max(bounds.midY, 50), width: 1, height: 1)
            popover.permittedArrowDirections = []
        }

        topVC.present(activityVC, animated: true)
    }

    private func generateDiagnosticsReport() -> String {
        let sections = buildDiagnosticSections(from: CemuBridge_GetInfo())
        var lines: [String] = []
        lines.append("=== 77U Diagnostics ===")
        lines.append("")
        for section in sections {
            lines.append("[\(section.title)]")
            for entry in section.entries {
                lines.append(entry.lineText)
            }
            lines.append("")
        }
        return lines.joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
    }
}

// Shared diagnostics model: the live developer panel and the plain-text
// "Copy report" export are both built from `buildDiagnosticSections(from:)`.

enum DiagnosticStatus {
    case normal
    case passed
    case failed
    case pending
    case secondary

    var color: Color {
        switch self {
        case .passed: return .green
        case .failed: return .red
        case .pending: return .orange
        case .secondary: return .secondary
        case .normal: return .primary
        }
    }
}

struct DiagnosticEntry: Identifiable {
    let id = UUID()
    let label: String
    let value: String
    let status: DiagnosticStatus

    var lineText: String {
        if label.isEmpty {
            return value
        }
        if value.isEmpty {
            return "\(label):"
        }
        return "\(label): \(value)"
    }
}

struct DiagnosticSection: Identifiable {
    let id = UUID()
    let title: String
    let entries: [DiagnosticEntry]
}

func buildDiagnosticSections(from bridgeInfo: CemuBridgeInfo) -> [DiagnosticSection] {
    var sections: [DiagnosticSection] = []
    let fileManager = FileManager.default

    let cpuMode: String
    switch CemuBridge_GetCPUDiagnosticMode() {
    case 1: cpuMode = "Interpreter (no JIT)"
    case 2: cpuMode = "Multi-core JIT"
    default: cpuMode = "Single-core JIT"
    }

    // Title
    let tb = bridgeInfo.titleBootResult
    var titleEntries: [DiagnosticEntry] = []
    let titleName = cString(tb.titleName)
    titleEntries.append(DiagnosticEntry(
        label: "Title Loaded",
        value: tb.titleLoaded ? (titleName.isEmpty ? "Yes" : titleName) : "No",
        status: tb.titleLoaded ? .passed : .normal
    ))
    titleEntries.append(DiagnosticEntry(label: "CPU Mode", value: cpuMode, status: .normal))
    if tb.titleLoaded {
        let format = cString(tb.detectedFormat)
        let launchMode = cString(tb.launchMode)
        let securityScope = cString(tb.securityScopeStatus)
        let controllerName = cString(tb.physicalControllerName)
        titleEntries.append(DiagnosticEntry(label: "Title ID", value: String(format: "0x%016llX", tb.titleId), status: .normal))
        if tb.hostFsValid && tb.appTitleId != 0 {
            titleEntries.append(DiagnosticEntry(label: "App Title ID", value: String(format: "0x%016llX", tb.appTitleId), status: .normal))
            titleEntries.append(DiagnosticEntry(label: "Base Title ID", value: String(format: "0x%016llX", tb.baseTitleId), status: .normal))
        }
        titleEntries.append(DiagnosticEntry(label: "Detected Format", value: format.isEmpty ? "Unknown" : format, status: .normal))
        titleEntries.append(DiagnosticEntry(label: "Launch Mode", value: launchMode.isEmpty ? "Auto" : launchMode, status: .normal))
        titleEntries.append(DiagnosticEntry(label: "Root / File Path", value: cString(tb.rpxPath), status: .secondary))
        titleEntries.append(DiagnosticEntry(
            label: "Security Scope Active",
            value: securityScope.isEmpty ? "Not Required" : securityScope,
            status: (securityScope == "Yes" || securityScope == "Not Required") ? .passed : (securityScope == "No" ? .failed : .normal)
        ))
        if tb.hostFsValid {
            titleEntries.append(DiagnosticEntry(
                label: "XML Metadata",
                value: "meta.xml=\(tb.metaXmlParsed ? "OK" : "Missing"), cos.xml=\(tb.cosXmlParsed ? "OK" : "Missing")",
                status: (tb.metaXmlParsed && tb.cosXmlParsed) ? .passed : .normal
            ))
        }
        titleEntries.append(DiagnosticEntry(label: "Title Prepared", value: tb.titlePrepared ? "Yes" : "Failed", status: tb.titlePrepared ? .passed : .failed))
        titleEntries.append(DiagnosticEntry(label: "Main Thread", value: tb.mainThreadCreated ? "Created (Core 1)" : "Pending", status: tb.mainThreadCreated ? .passed : .pending))
        titleEntries.append(DiagnosticEntry(label: "Title Launched", value: tb.titleLaunched ? "Yes (Running)" : "No", status: tb.titleLaunched ? .passed : .pending))
        titleEntries.append(DiagnosticEntry(label: "Scheduler Cores", value: "\(tb.schedulerCores > 0 ? tb.schedulerCores : 1)", status: .normal))
        titleEntries.append(DiagnosticEntry(label: "Active Threads", value: "\(tb.activeThreadsCount)", status: .normal))
        titleEntries.append(DiagnosticEntry(
            label: "Physical Controller",
            value: tb.physicalControllerConnected ? "Connected (\(controllerName.isEmpty ? "Game Controller" : controllerName))" : "None",
            status: tb.physicalControllerConnected ? .passed : .secondary
        ))
        titleEntries.append(DiagnosticEntry(label: "Draw Calls Processed", value: "\(tb.gx2DrawCount)", status: tb.gx2DrawCount > 0 ? .passed : .normal))
        let errorMessage = cString(tb.errorMessage)
        if !errorMessage.isEmpty {
            titleEntries.append(DiagnosticEntry(label: "Error", value: errorMessage, status: .failed))
        }
    }
    sections.append(DiagnosticSection(title: "Title", entries: titleEntries))

    // Device
    var deviceEntries: [DiagnosticEntry] = []
    let deviceName = bridgeInfo.metalDeviceName.map { String(cString: $0) } ?? "Unknown"
    deviceEntries.append(DiagnosticEntry(
        label: "Metal Device",
        value: bridgeInfo.metalAvailable ? deviceName : "Unavailable",
        status: bridgeInfo.metalAvailable ? .normal : .failed
    ))
    deviceEntries.append(DiagnosticEntry(
        label: "Layer Attached",
        value: bridgeInfo.metalLayerAttached ? "Yes" : "No",
        status: bridgeInfo.metalLayerAttached ? .passed : .failed
    ))
    if let version = bridgeInfo.cemuVersion {
        deviceEntries.append(DiagnosticEntry(label: "Core Version", value: String(cString: version), status: .secondary))
    }
    sections.append(DiagnosticSection(title: "Device", entries: deviceEntries))

    // Memory
    let memLimit = CemuBridge_GetMemoryLimitInfo()
    let ramGB = Double(memLimit.physicalMemoryBytes) / (1024.0 * 1024.0 * 1024.0)
    let ceilingGB = Double(memLimit.totalLimitBytes) / (1024.0 * 1024.0 * 1024.0)
    let signedAppID = cString(memLimit.signedApplicationIdentifier)
    sections.append(DiagnosticSection(title: "System & Process Memory", entries: [
        DiagnosticEntry(
            label: "Memory Capability Owner",
            value: memLimit.inLiveContainer ? "LiveContainer host" : "77U",
            status: .normal
        ),
        DiagnosticEntry(
            label: "Signed Process App ID",
            value: signedAppID.isEmpty ? "Unknown" : signedAppID,
            status: .normal
        ),
        DiagnosticEntry(
            label: "Increased Memory Limit",
            value: WelcomeMemoryMessage.activationStatus(entitlementPresent: memLimit.entitlementPresent,
                                                         statusKnown: memLimit.entitlementStatusKnown),
            status: memLimit.entitlementStatusKnown && memLimit.entitlementPresent ? .passed : .normal
        ),
        DiagnosticEntry(
            label: "Entitlement Status",
            value: !memLimit.entitlementStatusKnown ? "Unknown (signature unavailable)" : (memLimit.entitlementPresent ? "Present in app signature" : "Not present in app signature"),
            status: memLimit.entitlementPresent ? .passed : .secondary
        ),
        DiagnosticEntry(
            label: "Estimated Process Memory Allowance",
            value: "\(memLimit.totalLimitBytes / (1024 * 1024)) MB (\(String(format: "%.2f", ceilingGB)) GB)",
            status: .normal
        ),
        DiagnosticEntry(
            label: "Available Headroom",
            value: "\(memLimit.availableMemoryBytes / (1024 * 1024)) MB",
            status: .normal
        ),
        DiagnosticEntry(
            label: "Current Footprint",
            value: "\(memLimit.currentFootprintBytes / (1024 * 1024)) MB",
            status: .normal
        ),
        DiagnosticEntry(
            label: "Physical Device RAM",
            value: "\(memLimit.physicalMemoryBytes / (1024 * 1024)) MB (\(String(format: "%.1f", ramGB)) GB)",
            status: .normal
        )
    ]))

    // JIT
    let jitLogPath = String(cString: CemuBridge_GetJITDiagnosticsPath())
    var jitEntries: [DiagnosticEntry] = [
        DiagnosticEntry(label: "JIT Enabled", value: CemuBridge_IsJITEnabled() ? "Yes" : "No", status: CemuBridge_IsJITEnabled() ? .passed : .secondary),
        DiagnosticEntry(label: "JIT Memory Ready", value: CemuBridge_IsJITMemoryReady() ? "Yes" : "No", status: CemuBridge_IsJITMemoryReady() ? .passed : .secondary),
        DiagnosticEntry(label: "Debugger Attached", value: CemuBridge_IsXcodeDebuggerAttached() ? "Xcode" : (CemuBridge_HasJITDebugger() ? "JIT helper" : "No"), status: .normal),
        DiagnosticEntry(label: "Recent Diagnostics", value: "", status: .normal)
    ]
    var jitBuffer = [CChar](repeating: 0, count: 16384)
    CemuBridge_GetJITDiagnosticsRecent(&jitBuffer, 16384, 25)
    let jitLines = stringFromNullTerminatedBuffer(jitBuffer).components(separatedBy: "\n").filter { !$0.isEmpty }
    if jitLines.isEmpty {
        jitEntries.append(DiagnosticEntry(label: "", value: "(None yet)", status: .normal))
    } else {
        for line in jitLines {
            let isFailed = line.contains("FAILED")
            let isSuccess = line.contains("entered") || line.contains("succeeded") || line.contains("SESSION START")
            jitEntries.append(DiagnosticEntry(label: "", value: line, status: isFailed ? .failed : (isSuccess ? .passed : .normal)))
        }
    }
    sections.append(DiagnosticSection(title: "JIT", entries: jitEntries))

    // Performance
    let perf = CemuBridge_GetPerfStatsSnapshot()
    sections.append(DiagnosticSection(title: "Performance", entries: [
        DiagnosticEntry(label: "FPS", value: String(format: "%.1f", perf.fps), status: perf.fps > 0 ? .passed : .normal),
        DiagnosticEntry(label: "CPU Wait", value: String(format: "%.2f ms", perf.cpuWaitMs), status: .normal),
        DiagnosticEntry(label: "GPU Time", value: String(format: "%.2f ms", perf.gpuMs), status: .normal),
        DiagnosticEntry(label: "Draw Calls / Render Passes", value: "\(perf.drawCalls) / \(perf.renderPasses)", status: .normal)
    ]))

    // Graphic packs
    if tb.titleLoaded {
        var packEntries: [DiagnosticEntry] = []
        for index in 0..<CemuBridge_GetGraphicPackCount(tb.titleId) {
            var pack = CemuGraphicPackInfo()
            guard CemuBridge_GetGraphicPackInfo(tb.titleId, index, &pack), pack.enabled else { continue }
            packEntries.append(DiagnosticEntry(label: cString(pack.name), value: cString(pack.virtualPath), status: .normal))
        }
        if packEntries.isEmpty {
            packEntries.append(DiagnosticEntry(label: "", value: "(None enabled)", status: .normal))
        }
        sections.append(DiagnosticSection(title: "Graphic Packs", entries: packEntries))
    }

    // Logs
    let previousCrashPath = String(cString: CemuBridge_GetPreviousCrashReportPath())
    let previousCrashAvailable = !previousCrashPath.isEmpty && fileManager.fileExists(atPath: previousCrashPath)
    sections.append(DiagnosticSection(title: "Logs", entries: [
        DiagnosticEntry(label: "Main Log", value: String(cString: CemuBridge_GetMainLogPath()), status: .secondary),
        DiagnosticEntry(label: "JIT Log", value: jitLogPath, status: .secondary),
        DiagnosticEntry(label: "Previous Log Directory", value: String(cString: CemuBridge_GetPreviousLogDirectory()), status: .secondary),
        DiagnosticEntry(
            label: "Previous Crash Report",
            value: previousCrashAvailable ? previousCrashPath : "None",
            status: previousCrashAvailable ? .pending : .normal
        )
    ]))

    return sections
}

