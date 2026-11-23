import SwiftUI
import UniformTypeIdentifiers

struct WelcomeGuideView: View {
    var isFirstRun = false
    let finish: () -> Void
    @ObservedObject private var jit = JITCoordinator.shared
    @ObservedObject private var theme = MenuThemeStore.shared
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.verticalSizeClass) private var verticalSizeClass
    @State private var step = 0
    @State private var pairingError: String?
    @State private var importingPairing = false
    @State private var importingOnline = false
    @State private var onlineBusy = false
    @State private var onlineReady = false
    @State private var onlineMessage: String?
    @State private var saveError: String?
    @State private var hasCoverModel = GameArtworkEnhancer.hasNeuralModel
    @State private var installingCoverModel = false

    private var landscapePhone: Bool {
        UIDevice.current.userInterfaceIdiom == .phone && verticalSizeClass == .compact
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    if landscapePhone {
                        HStack(alignment: .top, spacing: 12) {
                            Image(systemName: stepIcon).font(.title2).foregroundStyle(.secondary)
                            introduction
                        }.padding(.vertical, 4)
                    } else {
                        VStack(alignment: .leading, spacing: 10) {
                            Image(systemName: stepIcon).font(.largeTitle).foregroundStyle(.secondary)
                            introduction
                        }.padding(.vertical, 12)
                    }
                }
                if step == 0 {
                    Section("Start here") {
                        Label("Add a game folder or WUA file from the library.", systemImage: "folder.badge.plus")
                        Label("Touch controls appear when no controller is connected.", systemImage: "hand.tap")
                        Label("Start with native graphics. First visits may stutter while shaders compile.", systemImage: "display")
                    }
                    WelcomeMemoryNotice(physicalMemoryBytes: ProcessInfo.processInfo.physicalMemory)
                    if GameArtworkEnhancer.isDeviceSupportedForNeuralEnhancement {
                        Section("Upscale game covers?") {
                            if hasCoverModel {
                                Label("Cover model available", systemImage: "checkmark.circle")
                            } else {
                                Text("Download the optional model now, or continue and install it later in Graphics settings.")
                                    .foregroundStyle(.secondary)
                                CoverModelDownloadButton(isInstalling: $installingCoverModel) {
                                    hasCoverModel = GameArtworkEnhancer.hasNeuralModel
                                }
                            }
                        }
                    }
                    Section {
                        Text("77U includes no games. Use game and system files you own.").foregroundStyle(.secondary)
                    }
                } else if step == 1 {
                    Section {
                        LabeledContent("Pairing file", value: jit.hasPairing ? jit.pairingName : "Not imported")
                        Button("Import pairing file…", systemImage: "doc.badge.plus") { importingPairing = true }
                            .disabled(jit.busy)
                            .accessibilityIdentifier("welcome.importPairing")
                        if let error = pairingError ?? jit.error { Text(error).foregroundStyle(.secondary).textSelection(.enabled) }
                        if jit.hasPairing { Label("Pairing file saved", systemImage: "checkmark.circle") }
                        NavigationLink("JIT setup and activation") { JITSettingsView() }
                    } header: { Text("This device") } footer: {
                        Text("Export the pairing file with iLoader, then import it here. Enable Developer Mode and configure LocalDevVPN. Your file stays on this device.")
                    }
                } else {
                    Section {
                        LabeledContent("Online files", value: onlineReady ? "Imported" : "Not ready")
                        Button("Import Wii U online files…", systemImage: "folder.badge.plus") { importingOnline = true }
                            .disabled(onlineBusy)
                            .accessibilityIdentifier("welcome.importOnline")
                        if onlineBusy { ProgressView("Importing online files…") }
                        if let onlineMessage { Text(onlineMessage).foregroundStyle(.secondary).textSelection(.enabled) }
                    } header: { Text("Pretendo") } footer: {
                        Text("Choose your Dumpling online-files folder with otp.bin, seeprom.bin and the account data. You can set up online play later in Settings.")
                    }
                    Section { Link("Pretendo setup guide", destination: URL(string: "https://pretendo.network/docs/install/cemu")!) }
                }
            }
            .listSectionSpacing(.compact)
            .contentMargins(.top, landscapePhone ? 10 : nil, for: .scrollContent)
            .navigationTitle(isFirstRun ? "Welcome to 77U" : "Setup")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItemGroup(placement: .topBarLeading) {
                    Button("Set up later") { complete() }
                        .disabled(onlineBusy)
                    if step > 0 {
                        Button("Back", systemImage: "chevron.left") { move(to: step - 1) }
                            .labelStyle(.iconOnly)
                            .disabled(onlineBusy)
                    }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button(step == 2 ? "Done" : "Next") {
                        if step == 2 { complete() } else { move(to: step + 1) }
                    }
                    .modifier(NativeGlassButton(prominent: true))
                    .disabled(onlineBusy)
                }
            }
            .animation(reduceMotion ? nil : .smooth(duration: 0.25), value: step)
        }
        .modifier(MenuThemeAppearance())
        .preferredColorScheme(theme.configuration.enabled ? theme.configuration.surfaceScheme : nil)
        .onAppear {
            jit.importPlacedPairingIfNeeded()
            let info = CemuBridge_GetOnlineServiceInfo()
            onlineReady = info.accountValid && info.onlineFilesValid
        }
        .fileImporter(isPresented: $importingPairing, allowedContentTypes: [.data, .propertyList, .xml], allowsMultipleSelection: false) { result in
            switch result {
            case .success(let urls): if let url = urls.first { pairingError = nil; jit.importPairing(url) }
            case .failure(let error):
                if (error as NSError).code != NSUserCancelledError { pairingError = "Couldn't open the pairing file: \(error.localizedDescription)" }
            }
        }
        .fileImporter(isPresented: $importingOnline, allowedContentTypes: [.folder], allowsMultipleSelection: false) { result in
            switch result {
            case .success(let urls): if let url = urls.first { importOnline(url) }
            case .failure(let error):
                if (error as NSError).code != NSUserCancelledError { onlineMessage = "Couldn't open the folder: \(error.localizedDescription)" }
            }
        }
        .alert("Couldn't save setup progress", isPresented: Binding(get: { saveError != nil }, set: { if !$0 { saveError = nil } })) {
            Button("Try again") { complete() }
            Button("Continue for now") { finish() }
        } message: { Text(saveError ?? "") }
    }

    private var stepIcon: String { step == 0 ? "rectangle.stack" : step == 1 ? "bolt" : "network" }

    private var introduction: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(step == 0 ? "Your Wii U library" : step == 1 ? "Set up JIT" : "Play online")
                .font(landscapePhone ? .headline : .title2.weight(.semibold))
            Text(step == 0 ? "Add your games, choose a cover and tap Play." : step == 1 ? "JIT runs games faster. Import a pairing file for this device." : "Import your Wii U online files to use Pretendo.")
                .font(landscapePhone ? .subheadline : .body)
                .foregroundStyle(.secondary)
        }
    }

    private func move(to step: Int) { self.step = step }
    private func complete() {
        do { if isFirstRun { try WelcomeProgress.complete() }; finish() }
        catch { saveError = error.localizedDescription }
    }
    private func importOnline(_ url: URL) {
        guard !onlineBusy else { return }
        onlineBusy = true
        onlineMessage = nil
        let scoped = url.startAccessingSecurityScopedResource()
        Task {
            let result = await Task.detached(priority: .userInitiated) { () -> (Bool, String) in
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                let result = url.path.withCString { CemuBridge_ImportPretendoDump($0) }
                return (result.success, result.success ? cString(result.accountName) : cString(result.errorMessage))
            }.value
            onlineBusy = false
            let info = CemuBridge_GetOnlineServiceInfo()
            onlineReady = info.accountValid && info.onlineFilesValid
            onlineMessage = result.0 ? "Imported for \(result.1). Choose Pretendo in Settings -> Online." : (result.1.isEmpty ? "Couldn't import these files. Choose the complete Dumpling online-files folder." : result.1)
        }
    }
}

/// The welcome guide and diagnostic previews render the same message.
struct WelcomeMemoryNotice: View {
    let physicalMemoryBytes: UInt64
    var showsActivationStatus = true
    @Environment(\.scenePhase) private var scenePhase
    @State private var memoryInfo = CemuBridge_GetMemoryLimitInfo()
    var body: some View {
        if WelcomeMemoryMessage.recommendsGetMoreRAM(physicalMemoryBytes: physicalMemoryBytes) {
            Section {
                Label("Games may run out of memory on devices with 6 GB of RAM or less.", systemImage: "memorychip")
                if showsActivationStatus {
                    LabeledContent("GetMoreRAM / Increased Memory Limit",
                                   value: WelcomeMemoryMessage.activationStatus(entitlementPresent: memoryInfo.entitlementPresent,
                                                                                statusKnown: memoryInfo.entitlementStatusKnown))
                        .accessibilityIdentifier("welcome.memoryActivationStatus")
                    LabeledContent("Memory controlled by", value: memoryInfo.inLiveContainer ? "LiveContainer host" : "77U")
                    if !cString(memoryInfo.signedApplicationIdentifier).isEmpty {
                        LabeledContent("Signed App ID", value: cString(memoryInfo.signedApplicationIdentifier))
                            .textSelection(.enabled)
                    }
                    Button("Refresh memory status", systemImage: "arrow.clockwise") {
                        memoryInfo = CemuBridge_GetMemoryLimitInfo()
                    }
                }
                Text("GetMoreRAM is optional but recommended. It can enable a higher app memory limit on supported devices. If Increased Memory Limit is already enabled for your installation, skip this step.")
                    .foregroundStyle(.secondary)
                Link("GetMoreRAM setup", destination: URL(string: "https://github.com/hugeBlack/GetMoreRam")!)
                Text(WelcomeMemoryMessage.setupInstructions(inLiveContainer: showsActivationStatus && memoryInfo.inLiveContainer))
                    .font(.footnote).foregroundStyle(.secondary)
            } header: { Text("Memory") } footer: {
                Text("Active means the capability is signed into this installation. Extra memory depends on device support and signing method; it does not add physical RAM. Unknown means the signature could not be checked.")
            }
            .accessibilityIdentifier("welcome.lowMemoryNotice")
            .onAppear { if showsActivationStatus { memoryInfo = CemuBridge_GetMemoryLimitInfo() } }
            .onChange(of: scenePhase) {
                if showsActivationStatus && scenePhase == .active { memoryInfo = CemuBridge_GetMemoryLimitInfo() }
            }
        }
    }
}
