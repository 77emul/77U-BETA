import SwiftUI
import AVFoundation
import UniformTypeIdentifiers
import ImageIO
import PhotosUI
import CoreTransferable

struct MenuThemeConfiguration: Codable, Equatable {
    private var gameArtwork: Bool?
    var useGameArtwork: Bool {
        get { gameArtwork ?? false }
        set { gameArtwork = newValue }
    }
    var reflectedBackground = false
    var enabled = false
    var accent = "56A8FF"
    var background = "101827"
    var surface = "202B40"
    var text = "F4F7FF"
    var playColor = "FFFFFF"
    var playText = "101010"
    var playTitle = "Play"
    var playIcon = "play.fill"
    var cornerRadius = 24.0
    var dimming = 0.35
    var volume = 0.35
    var image: String?
    var video: String?
    var music: String?
    var hasImportedBackground: Bool { image != nil || video != nil }
    var reflectsBackground: Bool { useGameArtwork || (image != nil && video == nil && reflectedBackground) }
    var hasMedia: Bool { useGameArtwork || hasImportedBackground }
    var surfaceScheme: ColorScheme { Self.luminance(surface) > 0.179 ? .light : .dark }
    private static let lightArtworkSurface = "F5F6FA"

    func libraryBackground(for scheme: ColorScheme) -> String {
        if hasMedia && scheme == .light { return Self.lightArtworkSurface }
        return enabled ? background : "101827"
    }

    func artworkDimming(for scheme: ColorScheme) -> Double {
        let amount = min(0.9, max(0, dimming))
        return scheme == .dark ? 0.60 + amount * 0.39 : max(0, amount - 0.35) * 0.35
    }

    func libraryInk(for scheme: ColorScheme) -> Color {
        if hasMedia && scheme == .light {
            return enabled ? Self.legibleColor(text, over: Self.lightArtworkSurface) : .black
        }
        if !enabled { return hasMedia ? .white : .primary }
        return Self.legibleColor(text, over: hasMedia ? "737373" : background)
    }

    var menuRows: Color {
        guard enabled else { return Color(uiColor: .secondarySystemGroupedBackground) }
        return Color(themeHex: surface).mix(with: .white, by: surfaceScheme == .light
            ? 0.25 : min(0.10, max(0, (0.16 - Self.luminance(surface)) * 0.8)))
    }

    var menuCanvas: Color {
        guard enabled else { return Color(uiColor: .systemGroupedBackground) }
        return menuRows.mix(with: .black, by: surfaceScheme == .light ? 0.10 : 0.40)
    }

    var menuSidebar: Color {
        surfaceScheme == .light
            ? menuRows.mix(with: .black, by: 0.15)
            : menuCanvas.mix(with: menuRows, by: 0.48)
    }

    var playInk: Color { Self.legibleColor(playText, over: playColor) }

    static func legibleColor(_ preferred: String, over background: String) -> Color {
        let foreground = luminance(preferred), base = luminance(background)
        let contrast = (max(foreground, base) + 0.05) / (min(foreground, base) + 0.05)
        if contrast >= 4.5 { return Color(themeHex: preferred) }
        return base > 0.179 ? .black : .white
    }

    private static func luminance(_ hex: String) -> Double {
        let rgb = UInt32(hex, radix: 16) ?? 0
        func linear(_ shift: UInt32) -> Double {
            let value = Double((rgb >> shift) & 255) / 255
            return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4)
        }
        return linear(16) * 0.2126 + linear(8) * 0.7152 + linear(0) * 0.0722
    }

    static let key = "77U.MenuTheme.v1"
    static func load(_ defaults: UserDefaults = .standard) -> Self {
        guard let data = defaults.data(forKey: key), var value = try? JSONDecoder().decode(Self.self, from: data) else { return Self() }
        let legacyPlayText = ["73E8D0": "08201D", "FFAF87": "291329", "ABA5FF": "111126", "56A8FF": "FFFFFF"]
        if let old = try? JSONSerialization.jsonObject(with: data) as? [String: Any], old["glass"] != nil,
           value.playColor == value.accent, legacyPlayText[value.playColor] == value.playText {
            value.playColor = "FFFFFF"
            value.playText = "101010"
        }
        return value
    }
}

extension Color {
    init(themeHex: String) {
        let value = UInt32(themeHex, radix: 16) ?? 0
        self.init(red: Double((value >> 16) & 255) / 255, green: Double((value >> 8) & 255) / 255, blue: Double(value & 255) / 255)
    }
    var themeHex: String {
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
        UIColor(self).getRed(&r, green: &g, blue: &b, alpha: &a)
        return String(format: "%02X%02X%02X", Int(r * 255), Int(g * 255), Int(b * 255))
    }
}

@MainActor
final class MenuThemeStore: ObservableObject {
    static let shared = MenuThemeStore()
    @Published var configuration = MenuThemeConfiguration.load() {
        didSet {
            if let data = try? JSONEncoder().encode(configuration) { UserDefaults.standard.set(data, forKey: MenuThemeConfiguration.key) }
            if oldValue.enabled != configuration.enabled || oldValue.video != configuration.video || oldValue.music != configuration.music || oldValue.volume != configuration.volume || oldValue.useGameArtwork != configuration.useGameArtwork { refreshPlayback() }
        }
    }
    let videoPlayer = AVQueuePlayer()
    private var looper: AVPlayerLooper?
    private var musicPlayer: AVAudioPlayer?
    private var loadedVideo: String?
    private enum MusicSource: Equatable {
        case imported(URL)
        case game(URL, Bool)
    }
    private var gameMusic: MusicSource?
    private var requestedMusic: MusicSource?
    private var playingMusic: MusicSource?
    private var outgoingMusicPlayer: AVAudioPlayer?
    private var crossfadeTask: Task<Void, Never>?
    private var musicLoad: Task<Void, Never>?
    private var fadeTask: Task<Void, Never>?
    private var gameRunning = false
    private var menuVisible = false
    private var appActive = true
    var usesThemeMusic: Bool {
        configuration.music != nil || configuration.useGameArtwork
    }
    func selectGame(_ url: URL?, isGameFile: Bool) {
        let source = url.map { MusicSource.game($0, isGameFile) }
        guard source != gameMusic else { return }
        gameMusic = source
        refreshPlayback()
    }
    var folder: URL { FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("MenuTheme", isDirectory: true) }
    func url(_ name: String?) -> URL? {
        guard let name, name == URL(fileURLWithPath: name).lastPathComponent else { return nil }
        return folder.appendingPathComponent(name)
    }
    func setMenuVisible(_ value: Bool) { guard menuVisible != value else { return }; menuVisible = value; refreshPlayback() }
    func setActive(_ value: Bool) { guard appActive != value else { return }; appActive = value; refreshPlayback() }
    func setGameRunning(_ value: Bool) { guard gameRunning != value else { return }; gameRunning = value; refreshPlayback() }
    func refreshAccessibility() { refreshPlayback() }
    private func refreshPlayback() {
        let config = configuration
        let running = menuVisible && appActive && !gameRunning
        let musicRunning = running && config.volume > 0
        if loadedVideo != config.video {
            videoPlayer.pause(); looper = nil; videoPlayer.removeAllItems()
            loadedVideo = config.video
            if let url = url(config.video) { looper = AVPlayerLooper(player: videoPlayer, templateItem: AVPlayerItem(url: url)) }
        }
        videoPlayer.isMuted = true
        if running && !config.useGameArtwork && !UIAccessibility.isReduceMotionEnabled { videoPlayer.play() } else { videoPlayer.pause() }
        let source = url(config.music).map(MusicSource.imported) ?? (config.useGameArtwork ? gameMusic : nil)
        if !musicRunning, musicLoad != nil {
            musicLoad?.cancel()
            musicLoad = nil
            requestedMusic = playingMusic
        }
        if musicRunning && requestedMusic != source {
            requestedMusic = source
            musicLoad?.cancel()
            musicLoad = nil
            if source != playingMusic {
                switch source {
                case .imported(let url):
                    replaceMusic(with: try? AVAudioPlayer(contentsOf: url), source: source)
                case .game(let url, let isGameFile):
                    musicLoad = Task { [weak self] in
                        do { try await Task.sleep(for: .milliseconds(360)) } catch { return }
                        let wave = await Task.detached(priority: .utility) {
                            GameBootArtwork.loadSound(titleURL: url, isGameFile: isGameFile)
                        }.value
                        guard !Task.isCancelled, let self else { return }
                        self.musicLoad = nil
                        let player = wave.flatMap { try? AVAudioPlayer(data: $0) }
                        self.replaceMusic(with: player, source: source)
                        self.refreshPlayback()
                    }
                case nil:
                    replaceMusic(with: nil, source: nil)
                }
            }
        }
        if !musicRunning { stopOutgoingMusic() }
        guard let player = musicPlayer else { fadeTask?.cancel(); fadeTask = nil; return }
        let volume = Float(min(1, max(0, config.volume)))
        if musicRunning {
            fadeTask?.cancel(); fadeTask = nil
            if !player.isPlaying { player.volume = 0; player.play() }
            player.setVolume(volume, fadeDuration: 0.45)
        } else if appActive && player.isPlaying {
            guard fadeTask == nil else { return }
            player.setVolume(0, fadeDuration: 0.45)
            fadeTask = Task { @MainActor [weak self] in
                do { try await Task.sleep(for: .milliseconds(470)) } catch { return }
                guard let self, self.musicPlayer === player else { return }
                self.fadeTask = nil
                player.pause()
            }
        } else { fadeTask?.cancel(); fadeTask = nil; player.pause() }
    }
    private func replaceMusic(with replacement: AVAudioPlayer?, source: MusicSource?) {
        stopOutgoingMusic()
        fadeTask?.cancel()
        fadeTask = nil
        let previous = musicPlayer
        musicPlayer = replacement
        playingMusic = source
        replacement?.numberOfLoops = -1
        if let previous, previous.isPlaying, menuVisible, appActive, !gameRunning {
            outgoingMusicPlayer = previous
            previous.setVolume(0, fadeDuration: 0.45)
            crossfadeTask = Task { [weak self] in
                do { try await Task.sleep(for: .milliseconds(470)) } catch { return }
                self?.stopOutgoingMusic()
            }
        } else {
            previous?.stop()
        }
    }

    private func stopOutgoingMusic() {
        crossfadeTask?.cancel()
        crossfadeTask = nil
        outgoingMusicPlayer?.stop()
        outgoingMusicPlayer = nil
    }

    func importMedia(_ source: URL, kind: String) async throws {
        let access = source.startAccessingSecurityScopedResource()
        defer { if access { source.stopAccessingSecurityScopedResource() } }
        try Self.checkMediaSize(source)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let name = UUID().uuidString + "." + source.pathExtension
        let destination = folder.appendingPathComponent(name)
        do {
            try await Task.detached(priority: .userInitiated) { try FileManager.default.copyItem(at: source, to: destination) }.value
            if kind == "image" {
                guard let image = CGImageSourceCreateWithURL(destination as CFURL, nil), CGImageSourceGetCount(image) > 0 else { throw CocoaError(.fileReadCorruptFile) }
            } else if kind == "music" { _ = try AVAudioPlayer(contentsOf: destination) }
            else {
                let asset = AVURLAsset(url: destination)
                let tracks = try await asset.loadTracks(withMediaType: .video)
                let playable = try await asset.load(.isPlayable)
                guard playable, !tracks.isEmpty else { throw CocoaError(.fileReadCorruptFile) }
            }
            var next = configuration
            switch kind {
            case "image": next.image = name; next.video = nil; next.useGameArtwork = false
            case "video": next.video = name; next.image = nil; next.useGameArtwork = false
            default: next.music = name
            }
            configuration = next
            removeUnusedMedia()
        } catch { try? FileManager.default.removeItem(at: destination); throw error }
    }

    nonisolated static func checkMediaSize(_ url: URL) throws {
        let size = try url.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
        guard size > 0, size <= 250 * 1024 * 1024 else { throw CocoaError(.fileReadTooLarge) }
    }

    func removeBackgroundMedia() {
        var next = configuration
        next.image = nil
        next.video = nil
        configuration = next
        removeUnusedMedia()
    }
    func removeUnusedMedia() {
        let referenced = [configuration.image, configuration.video, configuration.music].compactMap { $0 }
        for file in (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? [] {
            if !referenced.contains(file.lastPathComponent) { try? FileManager.default.removeItem(at: file) }
        }
    }
    func preset(_ name: String) {
        if name == "Original" { configuration.enabled = false; return }
        var value = configuration
        value.enabled = name != "Original"
        switch name {
        case "Aurora": value.accent = "73E8D0"; value.background = "0B2026"; value.surface = "173E45"
        case "Sunset": value.accent = "FFAF87"; value.background = "291329"; value.surface = "4A2849"
        case "Midnight": value.accent = "ABA5FF"; value.background = "111126"; value.surface = "28283F"
        default: break
        }
        value.text = "F4F7FF"
        value.playColor = "FFFFFF"
        value.playText = "101010"
        configuration = value
    }
}

private struct ThemeMediaSurface: UIViewRepresentable {
    let player: AVQueuePlayer
    let image: UIImage?
    let isVideo: Bool
    let reflected: Bool
    let animates: Bool
    final class Surface: UIView {
        let copies = CAReplicatorLayer()
        let still = CALayer()
        let video = AVPlayerLayer()
        var reflected = false
        var artwork: CGImage?
        override init(frame: CGRect) {
            super.init(frame: frame)
            layer.addSublayer(copies)
            copies.addSublayer(still)
            layer.addSublayer(video)
            // Mirror the still image across the split using top-left layer coordinates.
            copies.anchorPoint = .zero
            still.anchorPoint = .zero
            still.contentsGravity = .resizeAspectFill
            still.masksToBounds = true
            video.videoGravity = .resizeAspectFill
            video.isHidden = true
            clipsToBounds = true
        }
        required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
        override func layoutSubviews() {
            super.layoutSubviews()
            CATransaction.begin(); CATransaction.setDisableActions(true)
            copies.frame = bounds
            video.frame = bounds
            let height = reflected ? bounds.height / 2 : bounds.height
            still.frame = CGRect(x: 0, y: 0, width: bounds.width, height: height)
            copies.instanceCount = reflected ? 2 : 1
            copies.instanceTransform = CATransform3DConcat(CATransform3DMakeScale(1, -1, 1), CATransform3DMakeTranslation(0, height * 2, 0))
            CATransaction.commit()
        }
    }
    func makeUIView(context: Context) -> Surface { Surface() }
    func updateUIView(_ view: Surface, context: Context) {
        view.reflected = reflected
        let nextImage = isVideo ? nil : image?.cgImage
        if view.artwork !== nextImage {
            view.copies.removeAnimation(forKey: "artwork")
            if animates && !isVideo {
                let transition = CATransition()
                transition.type = .fade
                transition.duration = 0.4
                view.copies.add(transition, forKey: "artwork")
            }
        }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let nextPlayer = isVideo ? player : nil
        if view.video.player !== nextPlayer { view.video.player = nextPlayer }
        view.video.isHidden = !isVideo
        view.artwork = nextImage
        view.still.contents = nextImage
        CATransaction.commit()
        view.setNeedsLayout()
    }
    static func dismantleUIView(_ view: Surface, coordinator: ()) { view.video.player = nil }
}

struct MenuThemeBackground: View {
    @Environment(\.colorScheme) private var colorScheme
    var titleURL: URL? = nil
    var isGameFile = false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @ObservedObject private var theme = MenuThemeStore.shared
    @State private var bootArtwork: UIImage?
    @State private var importedArtwork: UIImage?
    private var requestID: String {
        theme.configuration.useGameArtwork ? "game:\(titleURL?.absoluteString ?? "none")" : "image:\(theme.configuration.image ?? "none")"
    }
    var body: some View {
        let config = theme.configuration
        let base = Color(themeHex: config.libraryBackground(for: colorScheme))
        GeometryReader { _ in
            ZStack {
                base
                ThemeMediaSurface(player: theme.videoPlayer,
                                  image: config.useGameArtwork ? bootArtwork : config.image != nil ? importedArtwork : nil,
                                  isVideo: config.video != nil && !config.useGameArtwork, reflected: config.reflectsBackground, animates: !reduceMotion)
                if config.useGameArtwork, bootArtwork != nil {
                    LinearGradient(stops: [
                        .init(color: .clear, location: 0.14),
                        .init(color: base.opacity(0.12), location: 0.36),
                        .init(color: base.opacity(0.34), location: 0.50),
                        .init(color: base.opacity(0.64), location: 0.62),
                        .init(color: base, location: 0.79)
                    ], startPoint: .top, endPoint: .bottom)
                } else if config.reflectsBackground && !config.useGameArtwork {
                    VStack(spacing: 0) {
                        Color.clear
                        Rectangle().fill(.ultraThinMaterial)
                            .mask(LinearGradient(colors: [.clear, .white, .white], startPoint: .top, endPoint: .bottom))
                            .overlay(LinearGradient(colors: [.clear, base.opacity(colorScheme == .dark ? 0.9 : 0.80)], startPoint: .top, endPoint: .bottom))
                    }
                }
                if config.hasMedia {
                    Color.black.opacity(config.artworkDimming(for: colorScheme))
                    if colorScheme == .light {
                        LinearGradient(stops: [
                            .init(color: .clear, location: 0.63),
                            .init(color: base.opacity(0.9), location: 0.91),
                            .init(color: base, location: 1)
                        ], startPoint: .top, endPoint: .bottom)
                    }
                }
            }.allowsHitTesting(false)
        }
        .ignoresSafeArea()
        .animation(reduceMotion ? nil : .smooth(duration: 0.3), value: base)
        .task(id: requestID) {
            let useGameArtwork = theme.configuration.useGameArtwork
            do { try await Task.sleep(for: .milliseconds(100)) } catch { return }
            let loaded: UIImage?
            if useGameArtwork {
                if let titleURL {
                    let gameFile = isGameFile
                    loaded = await Task.detached(priority: .utility) { GameBootArtwork.load(titleURL: titleURL, isGameFile: gameFile) }.value
                } else { loaded = nil }
            } else if let url = theme.url(theme.configuration.image) {
                loaded = await Task.detached(priority: .utility) {
                    guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
                          let cg = CGImageSourceCreateThumbnailAtIndex(source, 0, [kCGImageSourceCreateThumbnailFromImageAlways: true, kCGImageSourceThumbnailMaxPixelSize: 2048, kCGImageSourceCreateThumbnailWithTransform: true] as CFDictionary) else { return UIImage?.none }
                    return UIImage(cgImage: cg)
                }.value
            } else { loaded = nil }
            guard !Task.isCancelled else { return }
            if useGameArtwork { bootArtwork = loaded }
            else { importedArtwork = loaded }
        }
    }
}

private struct PhotosBackground: Transferable {
    let url: URL
    let kind: String

    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(importedContentType: .image) { try copy($0.file, kind: "image") }
        FileRepresentation(importedContentType: .movie) { try copy($0.file, kind: "video") }
    }

    private static func copy(_ source: URL, kind: String) throws -> Self {
        try MenuThemeStore.checkMediaSize(source)
        let name = UUID().uuidString + "." + source.pathExtension
        let destination = FileManager.default.temporaryDirectory.appendingPathComponent(name)
        try FileManager.default.copyItem(at: source, to: destination)
        return Self(url: destination, kind: kind)
    }
}

struct MenuThemeAppearance: ViewModifier {
    var sidebar = false
    @Environment(\.colorScheme) private var colorScheme
    @ObservedObject private var theme = MenuThemeStore.shared
    func body(content: Content) -> some View {
        let config = theme.configuration
        content.controllerButtonStyle(.automatic)
            .foregroundStyle(Color.primary)
            .tint(config.enabled ? MenuThemeConfiguration.legibleColor(config.accent, over: config.surface) : .accentColor)
            .environment(\.colorScheme, config.enabled ? config.surfaceScheme : colorScheme)
            .scrollContentBackground(config.enabled ? .hidden : .automatic)
            .background(config.enabled ? (sidebar ? config.menuSidebar : config.menuCanvas) : Color.clear)
    }
}

struct MenuThemeSettingsView: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @ObservedObject private var theme = MenuThemeStore.shared
    @State private var importer = false
    @State private var kind = "image"
    @State private var error: String?
    @State private var importing = false
    @State private var photoSelection: PhotosPickerItem?
    private func color(_ title: String, _ path: WritableKeyPath<MenuThemeConfiguration, String>) -> some View {
        ColorPicker(title, selection: Binding(get: { Color(themeHex: theme.configuration[keyPath: path]) }, set: { theme.configuration[keyPath: path] = $0.themeHex }), supportsOpacity: false)
    }
    var body: some View {
        Form {
            Section {
                Toggle("Custom colors and buttons", isOn: $theme.configuration.enabled).controllerToggle($theme.configuration.enabled)
                Text("Background media and music work with either color style.").font(.caption).foregroundStyle(.secondary)
            }.listRowBackground(theme.configuration.menuRows)
            Section("Templates") {
                ForEach(["Original", "Aurora", "Sunset", "Midnight"], id: \.self) { name in
                    Button { withAnimation(reduceMotion ? nil : .smooth(duration: 0.25)) { theme.preset(name) } } label: {
                        HStack(spacing: 12) {
                            Image(systemName: name == "Original" ? "circle.lefthalf.filled" : "paintpalette.fill")
                            Text(name).font(.headline)
                            Spacer()
                            let colors = name == "Aurora" ? ["73E8D0", "173E45", "0B2026"] : name == "Sunset" ? ["FFAF87", "4A2849", "291329"] : name == "Midnight" ? ["ABA5FF", "28283F", "111126"] : ["FFFFFF", "888888", "111111"]
                            HStack(spacing: -5) {
                                ForEach(colors, id: \.self) { value in Circle().fill(Color(themeHex: value)).frame(width: 20, height: 20).overlay(Circle().strokeBorder(.secondary.opacity(0.3))) }
                            }.accessibilityHidden(true)
                        }.padding(.vertical, 6)
                    }
                }
            }.listRowBackground(theme.configuration.menuRows)
            Section("Colors") {
                color("Accent & selection", \.accent); color("Background", \.background)
                color("Menu surfaces", \.surface); color("Text", \.text)
            }.listRowBackground(theme.configuration.menuRows)
            Section("Play button") {
                TextField("Label", text: $theme.configuration.playTitle)
                    .onChange(of: theme.configuration.playTitle) { _, value in theme.configuration.playTitle = String(value.prefix(32)) }
                Picker("Icon", selection: $theme.configuration.playIcon) {
                    ForEach(["play.fill", "gamecontroller.fill", "bolt.fill", "sparkles"], id: \.self) { Image(systemName: $0).tag($0) }
                }
                color("Button", \.playColor); color("Button text", \.playText)
                VStack(alignment: .leading) {
                    Text("Corner radius")
                    Slider(value: $theme.configuration.cornerRadius, in: 0...40)
                        .accessibilityLabel("Corner radius")
                }
                Label(theme.configuration.playTitle, systemImage: theme.configuration.playIcon)
                    .padding().background(Color(themeHex: theme.configuration.playColor), in: RoundedRectangle(cornerRadius: theme.configuration.cornerRadius))
                    .foregroundStyle(theme.configuration.playInk)
            }.listRowBackground(theme.configuration.menuRows)
            Section("Background media") {
                Toggle("Use selected game's boot screen", isOn: $theme.configuration.useGameArtwork).controllerToggle($theme.configuration.useGameArtwork)
                Text("Shows the selected game’s artwork above its reflection. Titles without boot artwork use the background color.").font(.caption)
                if theme.configuration.image != nil && theme.configuration.video == nil {
                    Toggle("Split background with reflection", isOn: Binding(
                        get: { theme.configuration.reflectsBackground },
                        set: { theme.configuration.reflectedBackground = $0 }
                    ))
                    .disabled(theme.configuration.useGameArtwork)
                }
                PhotosPicker(selection: $photoSelection, matching: .any(of: [.images, .videos])) {
                    Label("Choose photo or video…", systemImage: "photo.on.rectangle")
                }
                Button("Import picture from Files…") { kind = "image"; importer = true }
                Button("Import video from Files…") { kind = "video"; importer = true }
                if theme.configuration.hasImportedBackground {
                    Button("Remove imported background", role: .destructive) { theme.removeBackgroundMedia() }
                }
                VStack(alignment: .leading) {
                    Text("Background dimming")
                    Slider(value: $theme.configuration.dimming, in: 0...0.9)
                        .accessibilityLabel("Background dimming")
                }.disabled(!theme.configuration.hasMedia)
                Text("Videos are muted and pause during gameplay, in the background, and with Reduce Motion. Maximum import: 250 MB.").font(.caption)
            }.listRowBackground(theme.configuration.menuRows)
            Section("Menu music") {
                Button("Import MP3 or audio…") { kind = "music"; importer = true }
                VStack(alignment: .leading) {
                    Text("Music volume")
                    Slider(value: $theme.configuration.volume, in: 0...1)
                        .accessibilityLabel("Music volume")
                }
                Button("Remove music", role: .destructive) { theme.configuration.music = nil; theme.removeUnusedMedia() }
                Text("With game artwork enabled, the selected game’s boot sound loops in the library. Imported music overrides it.").font(.caption)
            }.listRowBackground(theme.configuration.menuRows)
            Section { Button("Restore original appearance") { theme.configuration.enabled = false } }.listRowBackground(theme.configuration.menuRows)
        }
        .disabled(importing)
        .overlay { if importing { ProgressView("Importing…").padding().background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16)) } }
        .navigationTitle("Theme & appearance")
        .modifier(MenuThemeAppearance())
        .onChange(of: photoSelection) { _, item in
            guard let item else { return }
            importing = true
            Task {
                defer { photoSelection = nil; importing = false }
                do {
                    guard let media = try await item.loadTransferable(type: PhotosBackground.self) else {
                        throw CocoaError(.fileReadUnknown)
                    }
                    defer { try? FileManager.default.removeItem(at: media.url) }
                    try await theme.importMedia(media.url, kind: media.kind)
                } catch { self.error = error.localizedDescription }
            }
        }
        .fileImporter(isPresented: $importer, allowedContentTypes: kind == "image" ? [.image] : kind == "video" ? [.movie] : [.audio]) { result in
            Task {
                importing = true
                defer { importing = false }
                do { try await theme.importMedia(result.get(), kind: kind) }
                catch CocoaError.userCancelled { }
                catch { self.error = error.localizedDescription }
            }
        }
        .alert("Couldn't import media", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) { Button("OK") { error = nil } } message: { Text(error ?? "") }
    }
}
