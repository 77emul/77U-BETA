import SwiftUI
import UIKit
import UniformTypeIdentifiers

private struct CemuAccountModel: Identifiable, Hashable {
    var id: UInt32 { persistentId }
    let persistentId: UInt32
    let miiName: String
    let accountId: String
    let isOnlineValid: Bool
    let isActive: Bool
    let selectedService: Int32

    var displayName: String {
        if !accountId.isEmpty {
            return "\(miiName) (\(accountId))"
        }
        return "\(miiName) (0x\(String(persistentId, radix: 16, uppercase: true)))"
    }
}

private enum AppSettingsPage: String, CaseIterable, Identifiable {
    case jit
    case general
    case theme
    case graphics
    case audio
    case online
    case about

    var id: Self { self }

    var title: String {
        switch self {
        case .jit: return "JIT activation"
        case .general: return "General"
        case .theme: return "Theme & appearance"
        case .graphics: return "Graphics"
        case .audio: return "Audio & controllers"
        case .online: return "Online"
        case .about: return "About"
        }
    }

    var icon: String {
        switch self {
        case .jit: return "bolt"
        case .general: return "gearshape"
        case .theme: return "paintpalette"
        case .graphics: return "sparkles.tv"
        case .audio: return "gamecontroller"
        case .online: return "network"
        case .about: return "info.circle"
        }
    }
}

struct AppSettingsView: View {
    @ObservedObject private var theme = MenuThemeStore.shared
    @Environment(\.dismiss) private var dismiss
    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.horizontalSizeClass) private var sizeClass
    let showDeveloperTools: () -> Void
    let systemColorScheme: ColorScheme
    @ObservedObject var library: GameLibraryStore
    @State private var gameFolderSelectionError: String?
    @State private var showCoverModelImporter = false
    @State private var showCoverModelReset = false
    @State private var coverModelMessage: String?
    @State private var installingCoverModel = false
    @State private var hasCoverModel = GameArtworkEnhancer.hasNeuralModel
    @State private var isGuidePresented = false
        @AppStorage("CemuIOS.LibraryStyle") private var libraryStyle = "covers"
    @AppStorage("CemuIOS.ShowFPSOverlay") private var showFPSOverlay = true
    @AppStorage("CemuIOS.ShowDRCPreview") private var showDRCPreview = false
    @AppStorage("CemuIOS.KeepGameReadyInBackground") private var keepGameReadyInBackground = true
    @AppStorage("CemuIOS.ShowDebugButton") private var showDebugButton = false
    @AppStorage("CemuIOS.ControllerMotionEnabled") private var controllerMotionEnabled = true
    @AppStorage("CemuIOS.DiagnosticLoggingEnabled") private var diagnosticLoggingEnabled = true
    @AppStorage("CemuIOS.ExperimentalSharedRAM") private var experimentalSharedRAM = true
    @AppStorage(GameArtworkEnhancer.preferenceKey) private var artworkEnhancementEnabled = true

    @State private var cpuModeSetting = Int(CemuBridge_GetCPUDiagnosticMode())
    @State private var memoryInfo = CemuBridge_GetMemoryLimitInfo()
    @State private var vsync = CemuBridge_GetSetting(CemuIOSSettingVSync) != 0
    @State private var asyncCompile = CemuBridge_GetSetting(CemuIOSSettingAsyncCompile) != 0
    @State private var showClearCacheConfirmation = false
    @State private var showClearCacheSuccessAlert = false
    @State private var showClearCacheFailureAlert = false
    @State private var isClearingShaderCaches = false


    @State private var fullscreenScaling = CemuBridge_GetSetting(CemuIOSSettingFullscreenScaling)
    @State private var playBootSound = CemuBridge_GetSetting(CemuIOSSettingPlayBootSound) != 0
    @State private var tvVolume = Double(CemuBridge_GetSetting(CemuIOSSettingTVVolume))
    @State private var padVolume = Double(CemuBridge_GetSetting(CemuIOSSettingPadVolume))
    @State private var microphoneVolume = Double(CemuBridge_GetSetting(CemuIOSSettingInputVolume))
    @State private var microphonePermission = CemuBridge_GetMicrophonePermissionStatus()
    @State private var networkService = CemuBridge_GetOnlineServiceInfo().selectedService
    @State private var onlineAccountName = ""
    @State private var onlineStatus = ""
    @State private var onlinePrerequisitesValid = false
    @State private var pretendoReady = false
    @State private var accounts: [CemuAccountModel] = []
    @State private var selectedAccountPersistentId: UInt32 = 0
    @State private var accountToDelete: CemuAccountModel?
    @State private var showDeleteAccountAlert = false
    @State private var isPretendoImporterPresented = false
    @State private var isImportingPretendo = false
    @State private var pretendoImportMessage: String?
    @State private var selectedPage: AppSettingsPage? = .general

    private var versionText: String {
        Bundle.main.object(forInfoDictionaryKey: "CemuIOSReleaseName") as? String
            ?? Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
            ?? "Unknown"
    }

    private var buildText: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "Unknown"
    }

    private var coreVersionText: String {
        guard let version = CemuBridge_GetCemuVersion() else { return "Unknown" }
        return String(cString: version)
    }

    private var memoryStatus: String {
        WelcomeMemoryMessage.activationStatus(entitlementPresent: memoryInfo.entitlementPresent,
                                               statusKnown: memoryInfo.entitlementStatusKnown)
    }

    private func memorySize(_ bytes: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(clamping: bytes), countStyle: .memory)
    }


    private func commit(_ setting: CemuIOSSetting, _ value: Int32) {
        CemuBridge_SetSetting(setting, value)
    }

    private func permissionText(_ status: Int32) -> String {
        switch status {
        case 2: return "Allowed"
        case 1: return "Denied in device Settings"
        default: return "Not requested"
        }
    }

    private func refreshOnlineInfo() {
        let info = CemuBridge_GetOnlineServiceInfo()
        networkService = info.selectedService
        onlinePrerequisitesValid = info.accountValid && info.onlineFilesValid
        pretendoReady = info.pretendoReady
        onlineAccountName = String(cString: CemuBridge_GetOnlineAccountName())
        onlineStatus = String(cString: CemuBridge_GetOnlineServiceStatus())

        var list: [CemuAccountModel] = []
        let count = CemuBridge_GetAccountCount()
        for i in 0..<count {
            var item = CemuAccountItem()
            if CemuBridge_GetAccountAtIndex(i, &item) {
                let miiName = withUnsafePointer(to: item.miiName) { ptr in
                    ptr.withMemoryRebound(to: CChar.self, capacity: 64) { String(cString: $0) }
                }
                let accountId = withUnsafePointer(to: item.accountId) { ptr in
                    ptr.withMemoryRebound(to: CChar.self, capacity: 64) { String(cString: $0) }
                }
                list.append(CemuAccountModel(
                    persistentId: item.persistentId,
                    miiName: miiName,
                    accountId: accountId,
                    isOnlineValid: item.isOnlineValid,
                    isActive: item.isActive,
                    selectedService: item.selectedService
                ))
            }
        }
        accounts = list
        selectedAccountPersistentId = info.persistentId
    }

    private func importPretendoDump(from selectedURL: URL) {
        let hasSecurityScope = selectedURL.startAccessingSecurityScopedResource()
        isImportingPretendo = true
        DispatchQueue.global(qos: .userInitiated).async {
            let importResult = selectedURL.path.withCString { sourcePath in
                CemuBridge_ImportPretendoDump(sourcePath)
            }
            DispatchQueue.main.async {
                if hasSecurityScope {
                    selectedURL.stopAccessingSecurityScopedResource()
                }
                isImportingPretendo = false
                if importResult.success {
                    let accountName = cString(importResult.accountName)
                    pretendoImportMessage = "Pretendo files imported. Account \(accountName) is selected and ready for the next game launch."
                    refreshOnlineInfo()
                } else {
                    let message = cString(importResult.errorMessage)
                    pretendoImportMessage = message.isEmpty ? "The Pretendo dump could not be imported." : message
                }
            }
        }
    }


    private func chooseDefaultGamesFolder() {
        DocumentPickerPresenter.shared.present(allowedTypes: [.folder]) { result in
            switch result {
            case .success(let url):
                do { try library.setDefaultGamesFolder(url) }
                catch { gameFolderSelectionError = error.localizedDescription }
            case .failure(let error):
                let code = (error as NSError).code
                if code != NSUserCancelledError && code != CocoaError.userCancelled.rawValue {
                    gameFolderSelectionError = error.localizedDescription
                }
            }
        }
    }

    var body: some View {
        Group {
            if sizeClass == .compact {
                NavigationStack {
                    List {
                        Section("Setup") {
                            NavigationLink("JIT activation") { settingsDetail(for: .jit) }
                        }
                        Section("Preferences") {
                            ForEach(AppSettingsPage.allCases.filter { $0 != .jit && $0 != .about }) { page in
                                NavigationLink { settingsDetail(for: page) } label: {
                                    Label(page.title, systemImage: page.icon)
                                }
                            }
                        }
                        Section { NavigationLink { settingsDetail(for: .about) } label: { Label("About 77U", systemImage: "info.circle") } }
                    }
                    .modifier(MenuThemeAppearance())
                    .listSectionSpacing(.compact)
                    .navigationTitle("Settings")
                    .navigationBarTitleDisplayMode(.inline)
                    .toolbar {
                        ToolbarItem(placement: .confirmationAction) {
                            Button("Done") {
                                dismiss()
                            }
                        }
                    }
                }
            } else {
                NavigationSplitView {
                    List(selection: $selectedPage) {
                        Section("Setup") {
                            sidebarLabel(for: .jit)
                        }
                        Section("Preferences") {
                            ForEach(AppSettingsPage.allCases.filter { $0 != .jit }) { page in
                                sidebarLabel(for: page)
                            }
                        }
                    }
                    .modifier(MenuThemeAppearance(sidebar: true))
                    .navigationSplitViewColumnWidth(min: 220, ideal: 240, max: 280)
                    .navigationTitle("Settings")
                    .navigationBarTitleDisplayMode(.large)
                } detail: {
                    NavigationStack {
                        settingsDetail(for: selectedPage ?? .general)
                    }
                }
                .navigationSplitViewStyle(.balanced)
            }
        }
        .modifier(MenuSheetPresentation(defaultColorScheme: systemColorScheme))
        .animation(reduceMotion ? nil : .smooth(duration: 0.22), value: selectedPage)
        .fullScreenCover(isPresented: $isGuidePresented, onDismiss: refreshOnlineInfo) {
            WelcomeGuideView { isGuidePresented = false }
        }
        .onAppear {
            memoryInfo = CemuBridge_GetMemoryLimitInfo()
            CemuBridge_SetControllerMotionEnabled(controllerMotionEnabled)
            microphonePermission = CemuBridge_GetMicrophonePermissionStatus()
            refreshOnlineInfo()
        }
        .onChange(of: scenePhase) {
            if scenePhase == .active { memoryInfo = CemuBridge_GetMemoryLimitInfo() }
        }
        .onChange(of: selectedPage) { memoryInfo = CemuBridge_GetMemoryLimitInfo() }
        .alert("Cover model", isPresented: Binding(
            get: { coverModelMessage != nil },
            set: { if !$0 { coverModelMessage = nil } }
        )) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(coverModelMessage ?? "")
        }
        .fileImporter(
            isPresented: $isPretendoImporterPresented,
            allowedContentTypes: [.folder],
            allowsMultipleSelection: false
        ) { result in
            guard case .success(let urls) = result, let selectedURL = urls.first else {
                if case .failure(let error) = result {
                    pretendoImportMessage = error.localizedDescription
                }
                return
            }
            importPretendoDump(from: selectedURL)
        }
        .animation(reduceMotion ? nil : .smooth(duration: 0.25), value: selectedPage)
        .alert("Online files", isPresented: Binding(
            get: { pretendoImportMessage != nil },
            set: { if !$0 { pretendoImportMessage = nil } }
        )) {
            Button("OK", role: .cancel) { pretendoImportMessage = nil }
        } message: {
            Text(pretendoImportMessage ?? "")
        }
        .alert("Couldn't set default folder", isPresented: Binding(
            get: { gameFolderSelectionError != nil },
            set: { if !$0 { gameFolderSelectionError = nil } }
        )) {
            Button("OK", role: .cancel) { gameFolderSelectionError = nil }
        } message: {
            Text(gameFolderSelectionError ?? "")
        }
        .alert("Delete account?", isPresented: $showDeleteAccountAlert, presenting: accountToDelete) { acc in
            Button("Delete", role: .destructive) {
                _ = CemuBridge_DeleteAccount(acc.persistentId)
                refreshOnlineInfo()
            }
            Button("Cancel", role: .cancel) {}
        } message: { acc in
            Text("Remove \(acc.displayName) from this device's Wii U storage?")
        }
    }

    @ViewBuilder
    private func settingsDetail(for page: AppSettingsPage) -> some View {
        if page == .theme {
            MenuThemeSettingsView()
        } else if page == .jit {
            JITSettingsView()
                .modifier(MenuThemeAppearance())
                .toolbar {
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Done") { dismiss() }
                    }
                }
        } else {
            preferencesDetail(for: page)
        }
    }

    private var settingsRowBackground: Color { theme.configuration.menuRows }

    private func sidebarLabel(for page: AppSettingsPage) -> some View {
        let selected = (selectedPage ?? .general) == page
        let config = theme.configuration
        return Label(page.title, systemImage: page.icon)
            .fontWeight(selected ? .semibold : .regular)
            .foregroundStyle(selected && config.enabled
                ? MenuThemeConfiguration.legibleColor(config.accent, over: config.surface)
                : Color.primary)
            .tag(page)
            .controllerAction { selectedPage = page }
    }

    private func preferencesDetail(for page: AppSettingsPage) -> some View {
        Form {
            if page == .general {
                Section {
                    Picker("Library view", selection: $libraryStyle) {
                        Text("Covers").tag("covers")
                        Text("Grid").tag("grid")
                    }
                    .accessibilityIdentifier("settings.libraryLayout")
                    LabeledContent("Default games folder", value: library.defaultFolderName)
                        .accessibilityIdentifier("settings.defaultGamesFolder")
                    Button("Choose default folder…", systemImage: "folder.badge.plus", action: chooseDefaultGamesFolder)
                        .accessibilityIdentifier("settings.chooseDefaultGamesFolder")
                    if library.hasDefaultGamesFolder {
                        Button("Open default folder", systemImage: "folder") { library.openDefaultGamesFolder() }
                    }
                    Button("Use 77U Games folder as default", systemImage: "arrow.uturn.backward") {
                        library.useAppGamesFolder()
                    }
                    if let error = library.defaultFolderError {
                        Text(error).font(.footnote).foregroundStyle(.secondary)
                    }
                } header: {
                    Text("Library")
                } footer: {
                    Text("Choose a folder with any name to open automatically when 77U starts. Keep your games there; they stay in place. Opening another game does not change this default. Until you choose a default, 77U remembers the last opened location.")
                }.listRowBackground(settingsRowBackground)
                Section("Emulation") {
                    Toggle("Shared game RAM (experimental)", isOn: $experimentalSharedRAM).controllerToggle($experimentalSharedRAM)
                        .disabled(!CemuBridge_CanManageSaves())
                        .accessibilityIdentifier("settings.sharedGuestRAM")
                    Text("Uses a separate helper for Wii U game memory. Falls back to standard memory if unavailable. Restart 77U after changing this setting. Memory-limit benefits and device stability are still being tested.")
                        .font(.caption).foregroundStyle(.secondary)
                    Text(CemuSharedRAMStatus())
                        .font(.caption).foregroundStyle(.secondary)
                    Picker("CPU mode", selection: $cpuModeSetting) {
                        Text("Multi-core JIT (recommended)").tag(2)
                        Text("Single-core JIT").tag(0)
                        Text("Without JIT (standard)").tag(1)
                    }
                    .onChange(of: cpuModeSetting) { _, newMode in
                        CemuBridge_SetCPUDiagnosticMode(Int32(newMode))
                    }

                    Toggle("Keep game ready when switching apps", isOn: $keepGameReadyInBackground).controllerToggle($keepGameReadyInBackground)
                    Text("Audio resumes when you return. iOS may suspend the game in the background.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }.listRowBackground(settingsRowBackground)

                Section("System") {
                    LabeledContent("Console language", value: "English")

                    Toggle("Play Wii U boot sound", isOn: $playBootSound).controllerToggle($playBootSound)
                        .onChange(of: playBootSound) {
                            commit(CemuIOSSettingPlayBootSound, playBootSound ? 1 : 0)
                            if !playBootSound { AppStartupSound.shared.stop() }
                        }
                    Text("Plays an available Wii U boot sound when the app opens. It uses your installed Wii U files.")
                        .font(.caption).foregroundStyle(.secondary)
                }.listRowBackground(settingsRowBackground)

            }

            if page == .graphics {
                Section("Rendering") {
                    Toggle("VSync", isOn: $vsync).controllerToggle($vsync)
                        .onChange(of: vsync) { commit(CemuIOSSettingVSync, vsync ? 1 : 0) }

                    Toggle("Compile shaders asynchronously", isOn: $asyncCompile).controllerToggle($asyncCompile)
                        .onChange(of: asyncCompile) { commit(CemuIOSSettingAsyncCompile, asyncCompile ? 1 : 0) }
                }.listRowBackground(settingsRowBackground)

                Section("Display") {
                    Toggle(isOn: $showFPSOverlay) {
                        Label("Show FPS", systemImage: "speedometer")
                    }.controllerToggle($showFPSOverlay)

                    Toggle(isOn: $showDRCPreview) {
                        Label("Show GamePad preview", systemImage: "rectangle.inset.filled")
                    }.controllerToggle($showDRCPreview)

                    Picker("Scaling", selection: $fullscreenScaling) {
                        Text("Keep aspect ratio").tag(Int32(0))
                        Text("Stretch").tag(Int32(1))
                    }
                    .onChange(of: fullscreenScaling) { commit(CemuIOSSettingFullscreenScaling, fullscreenScaling) }
                }.listRowBackground(settingsRowBackground)

                Section("Shader cache") {
                    Button(role: .destructive) {
                        showClearCacheConfirmation = true
                    } label: {
                        Label(isClearingShaderCaches ? "Clearing…" : "Clear shader cache", systemImage: "arrow.clockwise.circle")
                    }
                    .disabled(isClearingShaderCaches || !CemuBridge_CanManageSaves())
                    Text("Shaders will compile again after restarting 77U. Games and saves are kept.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }.listRowBackground(settingsRowBackground)

                Section("Cover enhancement") {
                    if GameArtworkEnhancer.isDeviceSupportedForNeuralEnhancement {
                        Toggle("Cover enhancement", isOn: $artworkEnhancementEnabled).controllerToggle($artworkEnhancementEnabled)
                        .onChange(of: artworkEnhancementEnabled) {
                            NotificationCenter.default.post(
                                name: GameArtworkEnhancer.preferenceDidChange,
                                object: nil
                            )
                        }
                        LabeledContent("Upscaling model", value: hasCoverModel ? "Available" : "Not installed")
                        if !hasCoverModel {
                            Text("Install a compatible cover model to enable neural upscaling. Basic scaling is available without a model.")
                                .font(.footnote).foregroundStyle(.secondary)
                            CoverModelDownloadButton(isInstalling: $installingCoverModel) {
                                hasCoverModel = GameArtworkEnhancer.hasNeuralModel
                            }
                        }
                        Button(installingCoverModel ? "Installing model…" : "Import cover model…", systemImage: "folder") {
                            showCoverModelImporter = true
                        }
                        .disabled(installingCoverModel)
                        .fileImporter(isPresented: $showCoverModelImporter,
                                      allowedContentTypes: [.folder, UTType(filenameExtension: "mlmodelc") ?? .package]) { result in
                            importCoverModel(result)
                        }
                        if hasCoverModel {
                            Button("Reset cover model…", systemImage: "trash", role: .destructive) {
                                showCoverModelReset = true
                            }
                            .disabled(installingCoverModel)
                        }
                        if GameArtworkEnhancer.hasBundledModel && GameArtworkEnhancer.ignoresBundledModel {
                            Button("Use included model", systemImage: "arrow.uturn.backward") {
                                GameArtworkEnhancer.useBundledModel(true)
                                refreshCoverModel()
                            }
                            .disabled(installingCoverModel)
                        }
                        if artworkEnhancementEnabled && hasCoverModel {
                            Text("Enhances covers while the library is open.")
                            .font(.footnote).foregroundStyle(.secondary)
                        }
                    } else {
                        LabeledContent("Neural enhancement", value: "Unavailable on this device")
                        Text("Devices with less than 6 GB of RAM use the built-in cover scaler to leave more memory available for games.")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                }
                .listRowBackground(settingsRowBackground)
                .confirmationDialog("Reset cover model?", isPresented: $showCoverModelReset, titleVisibility: .visible) {
                    Button("Reset cover model", role: .destructive) { resetCoverModel() }
                    Button("Cancel", role: .cancel) {}
                } message: {
                    Text("Downloaded or imported models will be removed. If 77U includes a model, it will be disabled so you can test setup again. Games and saves are kept.")
                }
            }

            if page == .audio {
                Section("Volume") {
                    SettingsVolumeRow(title: "TV volume", value: $tvVolume) {
                        commit(CemuIOSSettingTVVolume, Int32(tvVolume.rounded()))
                    }
                    SettingsVolumeRow(title: "GamePad volume", value: $padVolume) {
                        commit(CemuIOSSettingPadVolume, Int32(padVolume.rounded()))
                    }
                    SettingsVolumeRow(title: "Microphone gain", value: $microphoneVolume) {
                        commit(CemuIOSSettingInputVolume, Int32(microphoneVolume.rounded()))
                    }
                    Text("TV and GamePad volumes are independent. Both outputs use this device's speakers.")
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    LabeledContent("Microphone", value: permissionText(microphonePermission))
                    if microphonePermission != 2 {
                        Button("Allow microphone") {
                            CemuBridge_RequestMicrophonePermission()
                            Task { @MainActor in
                                try? await Task.sleep(for: .milliseconds(500))
                                microphonePermission = CemuBridge_GetMicrophonePermissionStatus()
                            }
                        }
                    }
                }.listRowBackground(settingsRowBackground)

                Section("Controller motion") {
                    Label("Connect an Xbox, PlayStation, Switch Pro or MFi controller through Bluetooth or USB.", systemImage: "gamecontroller")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                    Text("Pair controllers in device Settings. Use both Joy-Cons for a full set of controls.")
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    Toggle("Controller motion", isOn: $controllerMotionEnabled).controllerToggle($controllerMotionEnabled)
                        .onChange(of: controllerMotionEnabled) {
                            CemuBridge_SetControllerMotionEnabled(controllerMotionEnabled)
                        }

                    Button {
                        CemuBridge_RecenterControllerMotion()
                    } label: {
                        Label("Recenter motion", systemImage: "scope")
                    }
                }.listRowBackground(settingsRowBackground)
            }

            if page == .online {
                Section {
                    if !accounts.isEmpty {
                        Picker("Account", selection: $selectedAccountPersistentId) {
                            ForEach(accounts) { acc in
                                Text(acc.displayName).tag(acc.persistentId)
                            }
                        }
                        .onChange(of: selectedAccountPersistentId) { _, newPid in
                            _ = CemuBridge_SelectAccount(newPid)
                            refreshOnlineInfo()
                        }
                    } else {
                        LabeledContent("Account", value: onlineAccountName.isEmpty ? "Default" : onlineAccountName)
                    }

                    Picker("Service", selection: $networkService) {
                        Text("Offline").tag(Int32(0))
                        Text("Pretendo").tag(Int32(2))
                    }
                    .disabled(!onlinePrerequisitesValid)
                    .onChange(of: networkService) {
                        _ = CemuBridge_SetOnlineService(networkService)
                        refreshOnlineInfo()
                    }

                    LabeledContent("Status", value: pretendoReady ? "Ready" : "Not ready")
                    Text(onlineStatus)
                        .font(.footnote)
                        .foregroundStyle(.secondary)

                    if accounts.count > 1, let activeAcc = accounts.first(where: { $0.persistentId == selectedAccountPersistentId }) {
                        Button(role: .destructive) {
                            accountToDelete = activeAcc
                            showDeleteAccountAlert = true
                        } label: {
                            Label("Delete '\(activeAcc.miiName)'…", systemImage: "trash")
                        }
                    }

                    Button("Import Wii U online files…", systemImage: "person.crop.circle.badge.plus") {
                        isPretendoImporterPresented = true
                    }
                    .disabled(isImportingPretendo)
                    if isImportingPretendo {
                        ProgressView("Importing and validating…")
                    }
                    Link("Pretendo setup guide", destination: URL(string: "https://pretendo.network/docs/install/cemu")!)
                } header: {
                    Text("Online")
                } footer: {
                    Text("Account and service changes apply the next time you launch a game.")
                }
            }

            if page == .about {
                Section("About") {
                    LabeledContent("App", value: "77U")
                    LabeledContent("Version", value: versionText)
                    LabeledContent("Build", value: buildText)
                    LabeledContent("Cemu version", value: coreVersionText)
                }.listRowBackground(settingsRowBackground)
                Section("Device memory") {
                    LabeledContent("Device RAM", value: memorySize(memoryInfo.physicalMemoryBytes))
                    LabeledContent("App allowance", value: memorySize(memoryInfo.totalLimitBytes))
                    LabeledContent("GetMoreRAM / Increased Memory Limit", value: memoryStatus)
                    LabeledContent("Memory controlled by", value: memoryInfo.inLiveContainer ? "LiveContainer host" : "77U")
                    if !cString(memoryInfo.signedApplicationIdentifier).isEmpty {
                        LabeledContent("Signed App ID", value: cString(memoryInfo.signedApplicationIdentifier))
                            .textSelection(.enabled)
                    }
                    if !memoryInfo.increasedMemoryLimitActive {
                        Link("GetMoreRAM setup", destination: URL(string: "https://github.com/hugeBlack/GetMoreRam")!)
                        Text(WelcomeMemoryMessage.setupInstructions(inLiveContainer: memoryInfo.inLiveContainer))
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                    Button("Refresh memory status", systemImage: "arrow.clockwise") {
                        memoryInfo = CemuBridge_GetMemoryLimitInfo()
                    }
                    Text("Active means the capability is signed into this installation. Extra memory depends on device support; the allowance is an estimate.")
                        .font(.footnote).foregroundStyle(.secondary)
                }.listRowBackground(settingsRowBackground)
                Section("Licenses") {
                    NavigationLink("Licenses and acknowledgements") { JITLicensesView() }
                    Text("77U is an independent open-source port. It includes no games, keys, firmware, or system files and is not affiliated with Nintendo or the Cemu project.")
                        .font(.footnote).foregroundStyle(.secondary)
                }.listRowBackground(settingsRowBackground)
                Section("Credits") {
                    LabeledContent("Emulation core", value: "Cemu project")
                    LabeledContent("JIT integration", value: "StikJIT & StikDebug")
                    Text("Full credits are listed with each license.")
                        .font(.footnote).foregroundStyle(.secondary)
                }.listRowBackground(settingsRowBackground)
                Section("Advanced") {
                    DisclosureGroup("Diagnostics") {
                        Button {
                            isGuidePresented = true
                        } label: {
                            HStack {
                                Label("Welcome and imports", systemImage: "checklist")
                                Spacer()
                                Image(systemName: "chevron.right")
                                    .font(.system(size: 14, weight: .semibold))
                                    .foregroundStyle(.tertiary)
                            }
                            .contentShape(Rectangle())
                        }
                        .controllerButtonStyle(.plain)
                        .padding(.vertical, 4)
                        Toggle("Detailed logs", isOn: $diagnosticLoggingEnabled).controllerToggle($diagnosticLoggingEnabled)
                            .onChange(of: diagnosticLoggingEnabled) { _, enabled in
                                CemuBridge_SetDiagnosticLoggingEnabled(enabled)
                            }
                        Text("Records frame timings, loading stalls and performance reports. Off stops the extra recording and analysis. Save Debug Bundle includes the report.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Toggle("Show Diagnostics button in game", isOn: $showDebugButton).controllerToggle($showDebugButton)
                        Text("Adds a button for sharing logs and device information during play.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Button {
                            dismiss()
                            DispatchQueue.main.async { showDeveloperTools() }
                        } label: {
                            HStack {
                                Label("Open diagnostics", systemImage: "wrench.and.screwdriver")
                                Spacer()
                                Image(systemName: "chevron.right")
                                    .font(.system(size: 14, weight: .semibold))
                                    .foregroundStyle(.tertiary)
                            }
                            .contentShape(Rectangle())
                        }
                        .controllerButtonStyle(.plain)
                        .padding(.vertical, 4)
                    }
                }.listRowBackground(settingsRowBackground)
            }
        }
        .modifier(MenuThemeAppearance())
        .listSectionSpacing(.compact)
        .navigationTitle(page.title)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .confirmationAction) {
                Button("Done") {
                    dismiss()
                }
            }
        }
        .alert("Clear shader caches?", isPresented: $showClearCacheConfirmation) {
            Button("Clear caches", role: .destructive) {
                isClearingShaderCaches = true
                Task {
                    let cleared = await Task.detached(priority: .userInitiated) {
                        CemuBridge_ClearAllShaderCaches()
                    }.value
                    isClearingShaderCaches = false
                    if cleared {
                        showClearCacheSuccessAlert = true
                    } else {
                        showClearCacheFailureAlert = true
                    }
                }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("This removes shader and pipeline caches for all games. Restart 77U afterward. Shaders will compile again; saves are kept.")
        }
        .alert("Caches cleared", isPresented: $showClearCacheSuccessAlert) {
            Button("OK", role: .cancel) {}
        } message: {
            Text("Restart 77U before launching a game. Shaders will compile again on first visits.")
        }
        .alert("Couldn't clear caches", isPresented: $showClearCacheFailureAlert) {
            Button("OK", role: .cancel) {}
        } message: {
            Text("77U could not finish clearing the caches. Close and reopen 77U, then try again before starting a game.")
        }
    }

    private func importCoverModel(_ result: Result<URL, Error>) {
        installCoverModel(failureMessage: "The selected folder is not a verified 77U cover model. Choose Artwork-normal.mlmodelc or Artwork-quality.mlmodelc.") {
            try ArtworkModelArchive.install(result.get())
        }
    }

    private func resetCoverModel() {
        do {
            try ArtworkModelArchive.removeInstalledModels()
            if GameArtworkEnhancer.hasBundledModel { GameArtworkEnhancer.useBundledModel(false) }
            refreshCoverModel()
        } catch {
            coverModelMessage = "77U couldn't remove the installed cover model."
        }
    }

    private func refreshCoverModel() {
        GameArtworkEnhancer.releaseModel()
        hasCoverModel = GameArtworkEnhancer.hasNeuralModel
        NotificationCenter.default.post(name: GameArtworkEnhancer.preferenceDidChange, object: nil)
    }

    private func installCoverModel(failureMessage: String, operation: @escaping @Sendable () async throws -> Void) {
        guard !installingCoverModel else { return }
        installingCoverModel = true
        Task {
            defer { installingCoverModel = false }
            do {
                try await Task.detached(priority: .utility) { try await operation() }.value
                GameArtworkEnhancer.releaseModel()
                hasCoverModel = GameArtworkEnhancer.hasNeuralModel
                let wasEnabled = artworkEnhancementEnabled
                artworkEnhancementEnabled = true
                if wasEnabled {
                    NotificationCenter.default.post(name: GameArtworkEnhancer.preferenceDidChange, object: nil)
                }
                coverModelMessage = "Model installed. 77U will upscale eligible covers in the library."
            } catch CocoaError.userCancelled {
            } catch {
                coverModelMessage = failureMessage
            }
        }
    }
}

struct CoverModelDownloadButton: View {
    @Binding var isInstalling: Bool
    let onInstall: () -> Void
    @State private var showsLink = false
    @State private var link = ""
    private let defaultURL = (Bundle.main.object(forInfoDictionaryKey: "77UCoverModelDownloadURL") as? String)
        .flatMap { URL(string: $0) }
        .flatMap { ArtworkModelArchive.isHTTPSDownload($0) ? $0 : nil }
    @State private var error: String?

    var body: some View {
        Button("Download cover model…", systemImage: "arrow.down.circle") { showsLink = true }
            .disabled(isInstalling)
            .alert("Download cover model", isPresented: $showsLink) {
                if defaultURL == nil {
                    TextField("HTTPS download link", text: $link)
                        .keyboardType(.URL)
                        .textInputAutocapitalization(.never)
                }
                Button("Download and install", action: download)
                Button("Cancel", role: .cancel) {}
            } message: {
                if defaultURL == nil {
                    Text("Enter a direct HTTPS link to Artwork-normal.77umodel.")
                } else {
                    Text("77U will download the 1.3 MB model, verify it, and save it on this device.")
                }
            }
        if isInstalling { ProgressView("Installing cover model…") }
        if let error { Text(error).font(.footnote).foregroundStyle(.secondary) }
    }

    private func download() {
        guard let url = defaultURL ?? URL(string: link.trimmingCharacters(in: .whitespacesAndNewlines)),
              ArtworkModelArchive.isHTTPSDownload(url) else {
            error = "Enter a direct HTTPS link to the cover model."
            return
        }
        error = nil
        isInstalling = true
        Task {
            defer { isInstalling = false }
            do {
                try await Task.detached(priority: .utility) { try await ArtworkModelArchive.download(from: url) }.value
                GameArtworkEnhancer.releaseModel()
                UserDefaults.standard.set(true, forKey: GameArtworkEnhancer.preferenceKey)
                NotificationCenter.default.post(name: GameArtworkEnhancer.preferenceDidChange, object: nil)
                onInstall()
            } catch {
                self.error = defaultURL == nil
                    ? "Download failed. Check the link and try again."
                    : "The cover model couldn’t be downloaded. Try again later."
            }
        }
    }
}

private struct SettingsVolumeRow: View {
    let title: String
    @Binding var value: Double
    let commit: () -> Void

    var body: some View {
        VStack(spacing: 8) {
            HStack {
                Text(title)
                Spacer()
                Text("\(Int(value))%")
                    .monospacedDigit().foregroundStyle(.secondary)
            }
            Slider(value: $value, in: 0...100, step: 1) { editing in
                if !editing { commit() }
            }
            .controllerAdjustable($value, range: 0...100, step: 1, onCommit: commit)
            .accessibilityLabel(title)
            .accessibilityValue("\(Int(value)) percent")
        }.padding(.vertical, 4)
    }
}
