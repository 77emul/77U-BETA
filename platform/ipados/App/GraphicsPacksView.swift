import SwiftUI
import Foundation

private struct GraphicPackPreset: Identifiable, Hashable {
    var id: String { category + "\u{1f}" + name }
    let category: String
    let name: String
    let active: Bool
    let isDefault: Bool
}

private func isTVResolutionCategory(_ category: String) -> Bool {
    let normalized = category.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    return normalized == "resolution" || normalized == "tv resolution" || normalized == "screen resolution"
}

private struct GraphicPackItem: Identifiable, Hashable {
    let id: String
    let path: String
    let name: String
    let detail: String
    let category: String
    var enabled: Bool
    let compatible: Bool
    let universal: Bool
    let presets: [GraphicPackPreset]

    var tvResolutionPresets: [GraphicPackPreset] {
        // Older packs, including Mario 3D World, put TV resolution in the
        // unnamed preset group of a dedicated Graphics/Resolution pack.
        let dedicatedResolution = category == "Graphics"
            && isTVResolutionCategory(path.components(separatedBy: "/").last ?? "")
        return presets.filter {
            isTVResolutionCategory($0.category)
                || (dedicatedResolution && $0.category.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
        }
    }

    var isResolution: Bool {
        category == "Graphics" && !tvResolutionPresets.isEmpty
    }

    var activePresetSummary: String? {
        let names = presets.filter(\.active).map { "\($0.category): \($0.name)" }
        return names.isEmpty ? nil : names.joined(separator: " · ")
    }
}

private struct PackNotice: Identifiable {
    let id = UUID()
    let title: String
    let message: String
}

private struct PackInstallOutcome: Sendable {
    let success: Bool
    let count: UInt32
    let message: String
}

private struct PackMutationOutcome: Sendable {
    let success: Bool
    let message: String
}

private struct CommunityRelease: Decodable, Sendable {
    struct Asset: Decodable, Sendable {
        let name: String
        let downloadURL: URL
        enum CodingKeys: String, CodingKey {
            case name
            case downloadURL = "browser_download_url"
        }
    }
    let name: String?
    let tag: String
    let assets: [Asset]
    enum CodingKeys: String, CodingKey {
        case name, assets
        case tag = "tag_name"
    }
}

@MainActor
private final class GraphicsPacksModel: ObservableObject {
    enum Quality: String, CaseIterable, Identifiable {
        case native = "Native", performance = "Performance", balanced = "Balanced", quality = "Quality", ultra = "Ultra"
        var id: String { rawValue }
        var icon: String {
            switch self {
            case .native: "leaf"
            case .performance: "speedometer"
            case .balanced: "scale.3d"
            case .quality: "sparkles.tv"
            case .ultra: "sparkles.rectangle.stack"
            }
        }
        var detail: String {
            let isPhone = UIDevice.current.userInterfaceIdiom == .phone
            switch self {
            case .native: return "Game default (720p)"
            case .performance: return "960×540 • lowest GPU load"
            case .balanced: return isPhone ? "1280×720 • optimal iPhone 60 FPS" : "1600×900 • balanced effects"
            case .quality: return isPhone ? "1280×720 • optimal iPhone 60 FPS" : "1920×1080 • optimized effects"
            case .ultra: return isPhone ? "1280×720 • optimal iPhone 60 FPS" : "2560×1440 • highest detail"
            }
        }
    }

    @Published private(set) var packs: [GraphicPackItem] = []
    @Published private(set) var busy = false
    @Published private(set) var activity = ""
    @Published private(set) var selectedQuality: Quality = .native
    @Published var notice: PackNotice?
    @Published var confirmation: String?
    let titleID: UInt64

    init(titleID: UInt64) { self.titleID = titleID; reload() }

    func reload() {
        let count = CemuBridge_GetGraphicPackCount(titleID)
        var result: [GraphicPackItem] = []
        result.reserveCapacity(count)
        for index in 0..<count {
            var info = CemuGraphicPackInfo()
            guard CemuBridge_GetGraphicPackInfo(titleID, index, &info) else { continue }
            let identifier = cString(info.identifier)
            let presetCount = identifier.withCString { CemuBridge_GetGraphicPackPresetCount($0) }
            var presets: [GraphicPackPreset] = []
            for presetIndex in 0..<presetCount {
                var raw = CemuGraphicPackPresetInfo()
                let found = identifier.withCString { CemuBridge_GetGraphicPackPresetInfo($0, presetIndex, &raw) }
                guard found, raw.visible else { continue }
                presets.append(.init(category: cString(raw.category), name: cString(raw.name),
                                     active: raw.active, isDefault: raw.isDefault))
            }
            result.append(.init(id: identifier, path: cString(info.virtualPath), name: cString(info.name),
                                detail: cString(info.description), category: cString(info.category),
                                enabled: info.enabled, compatible: info.compatible,
                                universal: info.universal, presets: presets))
        }
        packs = result
        selectedQuality = detectedQuality(in: result)
    }

    func enable(_ enabled: Bool, id: String) async {
        guard !busy else { return }
        busy = true
        activity = enabled ? "Enabling pack…" : "Disabling pack…"
        defer { busy = false; activity = "" }
        let outcome = await setEnabled(enabled, id: id)
        guard outcome.success else {
            notice = .init(title: "Couldn't Change Pack", message: outcome.message)
            return
        }
        reload()
    }

    func select(_ preset: GraphicPackPreset, in id: String) async {
        guard !busy else { return }
        busy = true
        activity = "Applying preset…"
        defer { busy = false; activity = "" }
        guard let pack = packs.first(where: { $0.id == id }) else {
            notice = .init(title: "Pack unavailable", message: "Reload the graphic packs and try again.")
            return
        }
        if !pack.enabled {
            let enabled = await setEnabled(true, id: id)
            guard enabled.success else {
                notice = .init(title: "Couldn't Enable Pack", message: enabled.message)
                return
            }
        }
        let outcome = await setPreset(preset, id: id)
        guard outcome.success else {
            notice = .init(title: "Couldn't Change Preset", message: outcome.message)
            return
        }
        reload()
        guard packs.first(where: { $0.id == id })?.presets.contains(where: {
            $0.category == preset.category && $0.name == preset.name && $0.active
        }) == true else {
            notice = .init(title: "Preset Wasn't Applied", message: "77U could not verify the saved selection. Try updating the community packs.")
            return
        }
    }

    func apply(_ quality: Quality) async {
        guard !busy else { return }
        let resolutionPacks = packs.filter(\.isResolution)
        guard !resolutionPacks.isEmpty else {
            if quality == .native {
                selectedQuality = .native
                confirmation = "Native applied"
            } else {
                notice = .init(title: "Download Required", message: "Download the official community packs before selecting a higher resolution.")
            }
            return
        }
        busy = true
        activity = "Applying \(quality.rawValue) quality…"
        defer { busy = false; activity = "" }
        if quality == .native {
            for pack in resolutionPacks {
                guard let nativePreset = pack.tvResolutionPresets.first(where: \.isDefault)
                        ?? pack.tvResolutionPresets.min(by: {
                            abs((pixels(in: $0.name) ?? Int.max / 2) - 1280 * 720) <
                            abs((pixels(in: $1.name) ?? Int.max / 2) - 1280 * 720)
                        }) else { continue }
                let enabled = await setEnabled(true, id: pack.id)
                guard enabled.success else {
                    notice = .init(title: "Couldn't Apply Quality", message: enabled.message)
                    return
                }
                let selected = await setPreset(nativePreset, id: pack.id)
                guard selected.success else {
                    notice = .init(title: "Couldn't Apply Quality", message: selected.message)
                    return
                }
            }
			for pack in resolutionPacks {
				guard await configureAuxiliaryQuality(for: quality, in: pack) else { return }
			}
            reload()
            guard resolutionPacks.allSatisfy({ original in
                packs.first(where: { $0.id == original.id })?.tvResolutionPresets.contains(where: {
                    $0.active && ($0.isDefault || pixels(in: $0.name) == 1280 * 720)
                }) == true
            }) else {
                notice = .init(title: "Quality Wasn't Applied", message: "77U could not verify the native TV resolution.")
                return
            }
            confirmation = "Native applied"
            return
        }
        let isPhone = UIDevice.current.userInterfaceIdiom == .phone
        let target: Int
        switch quality {
        case .native: return
        case .performance: target = 960 * 540
        case .balanced: target = isPhone ? 1280 * 720 : 1_600 * 900
        case .quality: target = isPhone ? 1280 * 720 : 1_920 * 1_080
        case .ultra: target = isPhone ? 1280 * 720 : 2_560 * 1_440
        }
        let options = resolutionPacks.flatMap { pack in
            pack.tvResolutionPresets.compactMap { preset -> (GraphicPackItem, GraphicPackPreset, Int)? in
                guard let pixels = pixels(in: preset.name) else { return nil }
                return (pack, preset, pixels)
            }
        }
        guard let choice = options.min(by: { abs($0.2 - target) < abs($1.2 - target) }) else {
            notice = .init(title: "No Resolution Presets", message: "This game's pack does not expose a resolution preset.")
            return
        }
        for pack in resolutionPacks where pack.id != choice.0.id && pack.enabled {
            let outcome = await setEnabled(false, id: pack.id)
            guard outcome.success else {
                notice = .init(title: "Couldn't Apply Quality", message: outcome.message)
                return
            }
        }
        let enabled = await setEnabled(true, id: choice.0.id)
        guard enabled.success else {
            notice = .init(title: "Couldn't Apply Quality", message: enabled.message)
            return
        }
        let selected = await setPreset(choice.1, id: choice.0.id)
        guard selected.success else {
            notice = .init(title: "Couldn't Apply Quality", message: selected.message)
            return
        }
		guard await configureAuxiliaryQuality(for: quality, in: choice.0) else { return }
        reload()
        guard packs.first(where: { $0.id == choice.0.id })?.tvResolutionPresets.contains(where: {
            $0.category == choice.1.category && $0.name == choice.1.name && $0.active
        }) == true else {
            notice = .init(title: "Quality Wasn't Applied", message: "77U saved the request but could not verify the active TV resolution.")
            return
        }
        confirmation = "\(quality.rawValue) applied"
    }

    func downloadCommunityPacks() async {
        guard !busy else { return }
        busy = true
        defer { busy = false; activity = "" }
        do {
            activity = "Checking latest release…"
            let api = URL(string: "https://api.github.com/repos/cemu-project/cemu_graphic_packs/releases/latest")!
            var request = URLRequest(url: api)
            request.timeoutInterval = 30
            request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
            request.setValue("77U-iPad", forHTTPHeaderField: "User-Agent")
            let (data, response) = try await URLSession.shared.data(for: request)
            try validate(response)
            let release = try JSONDecoder().decode(CommunityRelease.self, from: data)
            guard let asset = release.assets.first(where: { $0.name.lowercased().hasSuffix(".zip") }) else {
                throw NSError(domain: "GraphicsPacks", code: 1,
                              userInfo: [NSLocalizedDescriptionKey: "The official release has no ZIP archive."])
            }
            activity = "Downloading \(asset.name)…"
            var assetRequest = URLRequest(url: asset.downloadURL)
            assetRequest.timeoutInterval = 180
            assetRequest.setValue("77U-iPad", forHTTPHeaderField: "User-Agent")
            let (download, downloadResponse) = try await downloadFile(
                for: assetRequest,
                status: "Downloading \(asset.name)"
            )
            try validate(downloadResponse)
            let staged = FileManager.default.temporaryDirectory
                .appendingPathComponent("77U-GraphicPacks-\(UUID().uuidString).zip")
            try FileManager.default.moveItem(at: download, to: staged)
            defer { try? FileManager.default.removeItem(at: staged) }

            activity = "Installing packs… 0s"
            let version = release.name ?? release.tag
            let installPulse = activityPulse(prefix: "Installing and scanning packs")
            let outcome = await install(staged, version: version)
            installPulse.cancel()
            guard outcome.success else {
                throw NSError(domain: "GraphicsPacks", code: 2,
                              userInfo: [NSLocalizedDescriptionKey: outcome.message])
            }
            reload()
            notice = .init(title: "Community Packs Ready",
                           message: "Installed \(version). Open a pack and turn it on; downloading alone does not enable new mods.")
        } catch {
            notice = .init(title: "Download Failed", message: error.localizedDescription)
        }
    }

    private func install(_ url: URL, version: String) async -> PackInstallOutcome {
        await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .utility).async {
                let raw = url.path.withCString { path in
                    version.withCString { CemuBridge_InstallGraphicPackArchive(path, $0) }
                }
                continuation.resume(returning: .init(success: raw.success,
                                                     count: raw.discoveredPackCount,
                                                     message: cString(raw.message)))
            }
        }
    }

    private func setEnabled(_ enabled: Bool, id: String) async -> PackMutationOutcome {
        await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                let raw = id.withCString { CemuBridge_SetGraphicPackEnabled($0, enabled) }
                continuation.resume(returning: .init(success: raw.success, message: cString(raw.message)))
            }
        }
    }

    private func setPreset(_ preset: GraphicPackPreset, id: String) async -> PackMutationOutcome {
        await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                let raw = id.withCString { idPointer in
                    preset.category.withCString { categoryPointer in
                        preset.name.withCString { namePointer in
                            CemuBridge_SetGraphicPackPreset(idPointer, categoryPointer, namePointer)
                        }
                    }
                }
                continuation.resume(returning: .init(success: raw.success, message: cString(raw.message)))
            }
        }
    }

    private func downloadFile(for request: URLRequest, status: String) async throws -> (URL, URLResponse) {
        let pulse = activityPulse(prefix: status)
        defer { pulse.cancel() }
        return try await URLSession.shared.download(for: request)
    }

    private func activityPulse(prefix: String) -> Task<Void, Never> {
        Task { @MainActor [weak self] in
            var seconds = 0
            while !Task.isCancelled {
                do { try await Task.sleep(for: .seconds(1)) } catch { return }
                guard !Task.isCancelled else { return }
                seconds += 1
                self?.activity = "\(prefix)… \(seconds)s"
            }
        }
    }

    private func validate(_ response: URLResponse) throws {
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            throw NSError(domain: "GraphicsPacks", code: 3,
                          userInfo: [NSLocalizedDescriptionKey: "The server returned an invalid response."])
        }
    }

    private func pixels(in text: String) -> Int? {
        let range = NSRange(text.startIndex..., in: text)
        if let regex = try? NSRegularExpression(pattern: #"(\d{3,4})\s*[x×]\s*(\d{3,4})"#),
           let match = regex.firstMatch(in: text, range: range),
           let wRange = Range(match.range(at: 1), in: text), let hRange = Range(match.range(at: 2), in: text),
           let width = Int(text[wRange]), let height = Int(text[hRange]) { return width * height }
        if let regex = try? NSRegularExpression(pattern: #"(\d{3,4})p"#, options: .caseInsensitive),
           let match = regex.firstMatch(in: text, range: range),
           let hRange = Range(match.range(at: 1), in: text), let height = Int(text[hRange]) {
            return Int((Double(height) * 16 / 9).rounded()) * height
        }
        return nil
    }

	private enum AuxiliaryQualityKind {
		case shadows, anisotropicFiltering, antiAliasing, drawDistance, bloom
	}

	private func auxiliaryKind(for category: String) -> AuxiliaryQualityKind? {
		let value = category.lowercased().replacingOccurrences(of: "-", with: " ")
		if value.contains("shadow") { return .shadows }
		if value.contains("anisotropic") { return .anisotropicFiltering }
		if value.contains("anti alias") || value.contains("antialias") { return .antiAliasing }
		if value.contains("draw distance") || value == "lod" || value.contains("level of detail") { return .drawDistance }
		if value.contains("bloom") { return .bloom }
		return nil
	}

	private func configureAuxiliaryQuality(for quality: Quality, in pack: GraphicPackItem) async -> Bool {
		let groups = Dictionary(grouping: pack.presets, by: \.category)
		for (category, options) in groups {
			guard let kind = auxiliaryKind(for: category),
				  let preset = auxiliaryPreset(for: quality, kind: kind, options: options),
				  !preset.active else { continue }
			let outcome = await setPreset(preset, id: pack.id)
			guard outcome.success else {
				notice = .init(title: "Couldn't Optimize Quality", message: outcome.message)
				return false
			}
		}
		return true
	}

	private func auxiliaryPreset(for quality: Quality, kind: AuxiliaryQualityKind,
								 options: [GraphicPackPreset]) -> GraphicPackPreset? {
		func named(_ fragments: [String]) -> GraphicPackPreset? {
			for fragment in fragments {
				if let match = options.first(where: { $0.name.localizedCaseInsensitiveContains(fragment) }) {
					return match
				}
			}
			return nil
		}
		let defaultPreset = options.first(where: \.isDefault)
			?? named(["default", "recommended", "normal", "medium", "100%"])
			?? options.first

		switch (quality, kind) {
		case (.performance, .shadows):
			return named(["off", "disabled", "low", "50%"] ) ?? defaultPreset
		case (.performance, .anisotropicFiltering):
			return named(["1x", "2x", "off", "disabled"]) ?? defaultPreset
		case (.performance, .antiAliasing):
			return named(["off", "disabled", "none", "fxaa"]) ?? defaultPreset
		case (.performance, .drawDistance):
			return named(["low", "performance", "default", "normal"]) ?? defaultPreset
		case (.balanced, .anisotropicFiltering):
			return named(["4x", "2x"]) ?? defaultPreset
		case (.quality, .anisotropicFiltering), (.ultra, .anisotropicFiltering):
			return named(["8x", "4x"]) ?? defaultPreset
		default:
			// Resolution remains the visible distinction at higher modes. Expensive
			// 200-400% shadow/LOD presets are never selected implicitly because they
			// can more than quadruple render work even on an M-series iPad.
			return defaultPreset
		}
	}


    private func detectedQuality(in packs: [GraphicPackItem]) -> Quality {
        let enabledResolutionPacks = packs.filter { $0.enabled && $0.isResolution }
        guard !enabledResolutionPacks.isEmpty else { return .native }
        let activePresets = enabledResolutionPacks.flatMap(\.tvResolutionPresets).filter(\.active)
        let activePixels = activePresets.compactMap { pixels(in: $0.name) }.max() ?? 0
        if UIDevice.current.userInterfaceIdiom == .phone {
            if activePixels < 700_000 { return .performance }
            return .native
        }
        if activePixels >= 3_000_000 { return .ultra }
        if activePixels >= 1_900_000 { return .quality }
        if activePixels >= 1_200_000 { return .balanced }
        if activePixels < 700_000 { return .performance }
        return .native
    }
}

struct GraphicsPacksView: View {
    let game: GameLibraryEntry
    @StateObject private var model: GraphicsPacksModel
    @State private var search = ""
    @State private var frameGenerationTarget: Int32 = 0
    @State private var frameGenerationFast = false
    @State private var jitIsOn = Int(CemuBridge_GetCPUDiagnosticMode()) != 1
    @Environment(\.dismiss) private var dismiss
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private var frameGenerationKey: String {
        "CemuIOS.FrameGenerationTarget.\(game.numericTitleID ?? 0)"
    }

    private var frameGenerationChoice: Binding<Int32> {
        Binding(
            get: { frameGenerationTarget },
            set: { newValue in
                let allowed: Int32 = (newValue == 30 || newValue == 60 || newValue == 120) ? newValue : 0
                frameGenerationTarget = allowed
                UserDefaults.standard.set(Int(allowed), forKey: frameGenerationKey)
            }
        )
    }

    private var frameGenerationFastKey: String {
        "CemuIOS.FrameGenerationFast.\(game.numericTitleID ?? 0)"
    }

    init(game: GameLibraryEntry) {
        self.game = game
        _model = StateObject(wrappedValue: .init(titleID: game.numericTitleID ?? 0))
    }

    private var filtered: [GraphicPackItem] {
        search.isEmpty ? model.packs : model.packs.filter {
            "\($0.name) \($0.detail) \($0.path) \($0.category)".localizedCaseInsensitiveContains(search)
        }
    }
    private var categories: [String] {
        let order = ["Graphics", "Enhancements", "Mods", "Workarounds", "Other"]
        return Array(Set(filtered.map(\.category))).sorted {
            (order.firstIndex(of: $0) ?? order.count) < (order.firstIndex(of: $1) ?? order.count)
        }
    }

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Picker("Resolution profile", selection: Binding(get: { model.selectedQuality }, set: { value in
                        Task { await model.apply(value) }
                    })) {
                        ForEach(GraphicsPacksModel.Quality.allCases) { quality in
                            Text(quality.rawValue).tag(quality)
                        }
                    }.disabled(model.busy)
                    Text(model.selectedQuality.detail).font(.caption).foregroundStyle(.secondary)
                } header: { Text("Graphics") } footer: {
                    Text("Profiles set resolution and related pack options. Changes apply after restarting the game.")
                }

                Section {
                    Picker("Frame generation", selection: frameGenerationChoice) {
                        Text("Off").tag(Int32(0))
                        Text("30 FPS").tag(Int32(30))
                        Text("60 FPS").tag(Int32(60))
                        Text("120 FPS").tag(Int32(120))
                    }
                    .pickerStyle(.menu)
                    Toggle("Fast mode (less GPU)", isOn: Binding(
                        get: { frameGenerationFast },
                        set: { value in
                            frameGenerationFast = value
                            UserDefaults.standard.set(value, forKey: frameGenerationFastKey)
                        }
                    ))
                    .disabled(frameGenerationTarget == 0)
                } header: {
                    Text("Frame generation")
                } footer: {
                    VStack(alignment: .leading, spacing: 6) {
                        Text("Requires JIT. The setting stays saved when playing without JIT.")
                        Text("Choose the game's actual frame rate, then restart the game.")
                        Text("Fast mode uses a cheaper motion search: more real FPS, slightly more artifacts. Applies immediately.")
                        if !jitIsOn {
                            Text("Currently playing without JIT. Frame generation will stay off.")
                                .foregroundStyle(.secondary)
                        }
                    }
                }

                if model.packs.isEmpty {
                    VStack(spacing: 12) {
                        Image(systemName: "shippingbox")
                            .font(.system(size: 32, weight: .medium))
                            .foregroundStyle(.secondary)
                        Text("No packs for \(game.title)")
                            .font(.headline)
                        Text("Download the community packs, then enable the options you want.")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                        Button("Download packs") { Task { await model.downloadCommunityPacks() } }
                            .modifier(NativeGlassButton(prominent: true))
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 28)
                    .listRowBackground(Color.clear)
                } else {
                    ForEach(categories, id: \.self) { category in
                        Section {
                            ForEach(filtered.filter { $0.category == category }) { pack in
                                NavigationLink(
                                    destination: PackDetailView(model: model, id: pack.id),
                                    label: { PackRow(pack: pack) }
                                )
                            }
                        } header: { Text(category) }
                    }
                }
            }
            .searchable(text: $search, prompt: "Search packs and mods")
            .onAppear {
                jitIsOn = Int(CemuBridge_GetCPUDiagnosticMode()) != 1
                frameGenerationTarget = CemuBridge_GetFrameGenerationTargetForTitle(game.numericTitleID ?? 0)
                frameGenerationFast = UserDefaults.standard.bool(forKey: frameGenerationFastKey)
            }
            .navigationTitle(game.title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button("Done") { dismiss() }.disabled(model.busy)
                }

                ToolbarItem(placement: .topBarTrailing) {
                    Button { Task { await model.downloadCommunityPacks() } } label: {
                        Label("Update packs", systemImage: "arrow.down.circle")
                    }.disabled(model.busy)
                }
            }
            .overlay {
                if model.busy {
                    VStack(spacing: 12) {
                        ProgressView().controlSize(.large)
                        Text(model.activity).font(.subheadline.weight(.medium))
                        Text("Keep this screen open until installation finishes.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                        .padding(.horizontal, 24).padding(.vertical, 18)
                        .modifier(LibraryGlassSurface(cornerRadius: 24))
                }
            }
            .overlay(alignment: .bottom) {
                if let confirmation = model.confirmation {
                    Label(confirmation, systemImage: "checkmark.circle.fill")
                        .font(.subheadline.weight(.semibold))
                        .padding(.horizontal, 14).padding(.vertical, 10)
                        .modifier(LibraryGlassSurface(cornerRadius: 24))
                        .padding(.bottom, 18)
                        .transition(.move(edge: .bottom).combined(with: .opacity))
                        .allowsHitTesting(false)
                        .task(id: confirmation) {
                            try? await Task.sleep(for: .seconds(1.4))
                            withAnimation(.easeOut(duration: 0.2)) { model.confirmation = nil }
                        }
                }
            }
        }
        .modifier(MenuSheetPresentation())
        .animation(reduceMotion ? nil : .smooth(duration: 0.2), value: model.confirmation)
        .interactiveDismissDisabled(model.busy)
        .alert(item: $model.notice) { Alert(title: Text($0.title), message: Text($0.message), dismissButton: .default(Text("OK"))) }
    }
}

private struct PackRow: View {
    let pack: GraphicPackItem
    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: icon).font(.system(size: 17, weight: .semibold)).frame(width: 34, height: 34)
                .foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(pack.name.isEmpty ? pack.path : pack.name).font(.body.weight(.medium))
                    if isRecommendedPack {
                        Text("Recommended (60 FPS)")
                            .font(.caption2.weight(.bold))
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(Color.green.opacity(0.18))
                            .foregroundStyle(Color.green)
                            .clipShape(Capsule())
                    }
                }
                if let active = pack.activePresetSummary {
                    Text(active).font(.caption).foregroundStyle(.secondary).lineLimit(2)
                } else if !pack.detail.isEmpty {
                    Text(pack.detail).font(.caption).foregroundStyle(.secondary).lineLimit(2)
                }
            }
            Spacer()
            if !pack.compatible { Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.secondary) }
            else if pack.enabled {
                Image(systemName: "checkmark").foregroundStyle(.secondary).accessibilityLabel("Enabled")
            }
        }.padding(.vertical, 3)
    }
    private var isRecommendedPack: Bool {
        pack.path.contains("FPS++") || pack.name.contains("FPS++")
    }
    private var icon: String {
        switch pack.category {
        case "Graphics": "sparkles.tv"
        case "Enhancements": "wand.and.stars"
        case "Mods": "puzzlepiece.extension"
        case "Workarounds": "wrench.adjustable"
        default: "shippingbox"
        }
    }
}

private struct PackDetailView: View {
    @ObservedObject var model: GraphicsPacksModel
    let id: String
    private var pack: GraphicPackItem? { model.packs.first(where: { $0.id == id }) }
    var body: some View {
        Form {
            if let pack {
                Section {
                    Toggle("Enabled", isOn: Binding(get: { pack.enabled }, set: { enabled in
                        Task { await model.enable(enabled, id: id) }
                    })).controllerToggle(Binding(get: { pack.enabled }, set: { enabled in
                        Task { await model.enable(enabled, id: id) }
                    }))
                        .disabled(!pack.compatible || model.busy)
                    if pack.universal { LabeledContent("Applies to", value: "All games") }
                    LabeledContent("Metal compatibility", value: pack.compatible ? "Compatible" : "Not compatible")
                } header: { Text(pack.name.isEmpty ? pack.path : pack.name) }
                  footer: { if !pack.detail.isEmpty { Text(pack.detail) } }
                ForEach(categories(pack), id: \.self) { category in
                    let options = pack.presets.filter { $0.category == category }
                    Section(category.isEmpty ? "Preset" : category) {
                        Picker("Selection", selection: binding(pack, category, options)) {
                            ForEach(options) { Text($0.name).tag($0.name) }
                        }.pickerStyle(.inline).labelsHidden().disabled(!pack.compatible || model.busy)
                    }
                }
            } else {
                VStack(spacing: 12) {
                    Image(systemName: "shippingbox")
                        .font(.system(size: 32, weight: .medium))
                        .foregroundStyle(.secondary)
                    Text("Pack unavailable").font(.headline)
                    Text("Download or rescan the community packs again.")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 28)
                .listRowBackground(Color.clear)
            }
        }
        .navigationTitle(pack?.name ?? "Graphic Pack").navigationBarTitleDisplayMode(.inline)
        .overlay(alignment: .bottom) {
            if model.busy {
                HStack(spacing: 8) {
                    ProgressView()
                    Text(model.activity)
                }
                .font(.footnote.weight(.medium))
                .padding(.horizontal, 14).padding(.vertical, 10)
                .modifier(LibraryGlassSurface(cornerRadius: 24))
                .padding(.bottom, 12)
            }
        }
    }
    private func categories(_ pack: GraphicPackItem) -> [String] {
        var seen = Set<String>()
        return pack.presets.compactMap { seen.insert($0.category).inserted ? $0.category : nil }
    }
    private func binding(_ pack: GraphicPackItem, _ category: String,
                         _ options: [GraphicPackPreset]) -> Binding<String> {
        Binding(get: {
            options.first(where: \.active)?.name ?? options.first(where: \.isDefault)?.name ?? options.first?.name ?? ""
        }, set: { name in
            if let preset = options.first(where: { $0.name == name }) {
                Task { await model.select(preset, in: pack.id) }
            }
        })
    }
}
