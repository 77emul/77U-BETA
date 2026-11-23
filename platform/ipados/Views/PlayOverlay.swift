import SwiftUI
import GameController
import AVFoundation

enum TouchVisibility: String, Codable, CaseIterable { case automatic, always, hidden }
enum PlayMotionMode: String, Codable, CaseIterable { case off, automatic, ipad }
enum PlayDisplayMode: String, Codable, CaseIterable { case overlay, split, gamepad }
enum ExternalDisplayLayout: String, Codable, CaseIterable { case tvOnly, tvAndGamePad }
enum PadPreviewPosition: String, Codable, CaseIterable { case top, left, right, bottom }

struct PadPreviewAnchor: Codable, Equatable {
    var x: Double
    var y: Double
}

struct PlayConfiguration: Codable, Equatable {
    var layout = "centered"
    var visibility = TouchVisibility.automatic
    var scale = 1.0
    var opacity = 0.7
    var reach = 0.0
    var floatingSticks = false
    var haptics = true
    var motion = PlayMotionMode.automatic
    var motionSensitivity = 1.0
    var display = PlayDisplayMode.overlay
    var previewPosition = PadPreviewPosition.bottom
    var previewScale = 1.0
    var previewAnchor: PadPreviewAnchor?
    var nativeDisplayResolution = false
    var externalDisplayLayout = ExternalDisplayLayout.tvOnly
    var externalTouchControls = true
    var keepAwake = true
    var buttonSpacing = 1.0
    var stickTravel = 72.0
    var customPositions: [String: [String: TouchControlOffset]] = [:]

    init() {}
    private enum CodingKeys: String, CodingKey { case layout, visibility, scale, opacity, reach, floatingSticks, haptics, motion, motionSensitivity, display, previewPosition, previewScale, nativeDisplayResolution, externalDisplayLayout, externalTouchControls, keepAwake, buttonSpacing, stickTravel, customPositions, previewAnchor }

    init(from decoder: Decoder) throws {
        self.init()
        let values = try decoder.container(keyedBy: CodingKeys.self)
        layout = (try? values.decode(String.self, forKey: .layout)) ?? layout
        visibility = (try? values.decode(TouchVisibility.self, forKey: .visibility)) ?? visibility
        scale = (try? values.decode(Double.self, forKey: .scale)) ?? scale
        opacity = (try? values.decode(Double.self, forKey: .opacity)) ?? opacity
        reach = (try? values.decode(Double.self, forKey: .reach)) ?? reach
        floatingSticks = (try? values.decode(Bool.self, forKey: .floatingSticks)) ?? floatingSticks
        haptics = (try? values.decode(Bool.self, forKey: .haptics)) ?? haptics
        motion = (try? values.decode(PlayMotionMode.self, forKey: .motion)) ?? motion
        motionSensitivity = (try? values.decode(Double.self, forKey: .motionSensitivity)) ?? motionSensitivity
        display = (try? values.decode(PlayDisplayMode.self, forKey: .display)) ?? display
        previewPosition = (try? values.decode(PadPreviewPosition.self, forKey: .previewPosition)) ?? previewPosition
        previewScale = (try? values.decode(Double.self, forKey: .previewScale)) ?? previewScale
        nativeDisplayResolution = (try? values.decode(Bool.self, forKey: .nativeDisplayResolution)) ?? nativeDisplayResolution
        externalDisplayLayout = (try? values.decode(ExternalDisplayLayout.self, forKey: .externalDisplayLayout)) ?? externalDisplayLayout
        externalTouchControls = (try? values.decode(Bool.self, forKey: .externalTouchControls)) ?? externalTouchControls
        keepAwake = (try? values.decode(Bool.self, forKey: .keepAwake)) ?? keepAwake
        buttonSpacing = (try? values.decode(Double.self, forKey: .buttonSpacing)) ?? buttonSpacing
        stickTravel = (try? values.decode(Double.self, forKey: .stickTravel)) ?? stickTravel
        customPositions = (try? values.decode([String: [String: TouchControlOffset]].self, forKey: .customPositions)) ?? customPositions
        previewAnchor = try? values.decode(PadPreviewAnchor.self, forKey: .previewAnchor)
        previewScale = min(1.4, max(0.7, previewScale))
    }
}

@MainActor
final class PlayPreferences: ObservableObject {
    @Published var editingPreview = false
    private let defaults: UserDefaults
    @Published var configuration: PlayConfiguration {
        didSet {
            guard configuration != oldValue else { return }
            if let data = try? JSONEncoder().encode(configuration) {
                defaults.set(data, forKey: "CemuIOS.PlayConfiguration.v2")
            }
        }
    }
    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        if let data = defaults.data(forKey: "CemuIOS.PlayConfiguration.v2"),
           let value = try? JSONDecoder().decode(PlayConfiguration.self, from: data) {
            configuration = value
            if configuration.layout != TouchLayout.centered.rawValue {
                configuration.layout = TouchLayout.centered.rawValue
            }
        } else {
            configuration = PlayConfiguration()
            if defaults.object(forKey: "CemuIOS.ControllerMotionEnabled") != nil,
               !defaults.bool(forKey: "CemuIOS.ControllerMotionEnabled") {
                configuration.motion = .off
            }
        }
        if !defaults.bool(forKey: "CemuIOS.ReleaseDefaults.v1") {
            configuration.previewPosition = .bottom
            configuration.previewAnchor = nil
            defaults.set(false, forKey: "CemuIOS.ShowDRCPreview")
            defaults.set(true, forKey: "CemuIOS.ReleaseDefaults.v1")
            if let data = try? JSONEncoder().encode(configuration) {
                defaults.set(data, forKey: "CemuIOS.PlayConfiguration.v2")
            }
        }
    }
}

private struct PlayInputState: Equatable {
    var touch: Bool
    var deviceMotion: Bool
    var controllerMotion: Bool
    var keepAwake: Bool
    var sensitivity: Double
}

private enum PlaySheet: String, Identifiable {
    case controls
    var id: String { rawValue }
}

struct PlayOverlay: View {
    @ObservedObject var preferences: PlayPreferences
    @ObservedObject var input: TouchInputSession
    @ObservedObject private var externalDisplay = ExternalDisplayCoordinator.shared
    @Binding var showFPS: Bool
    @Binding var showGamePad: Bool
    @Binding var showDebugButton: Bool
    var showDiagnostics: () -> Void
    var onShareDiagnostics: () -> Void = {}
    var onExitGame: () -> Void = {}
    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var sheet: PlaySheet?
    @State private var connectionNotice: String?
    @State private var debugNotice: String?
    @State private var showsDebugFeedback = false
    @State private var showsExitConfirmation = false
    @State private var controllerAudioRouteTask: Task<Void, Never>?
    @State private var previousIdleTimerDisabled = false
    @AppStorage("CemuIOS.HasSeenInGameGuide.v1") private var hasSeenInGameGuide = false
    @State private var showsInGameGuide = false
    @State private var softwareKeyboardActive = false
    @State private var softwareKeyboardText = ""
    @State private var softwareKeyboardMaxLen = 16

    private var config: PlayConfiguration { preferences.configuration }
    private var showsTouch: Bool {
        if softwareKeyboardActive { return false }
        if externalDisplay.isConnected && config.externalTouchControls { return true }
        return config.visibility == .always || (config.visibility == .automatic && !input.controllerConnected)
    }

    private var inputState: PlayInputState {
        let sceneActive = scenePhase == .active
        let acceptsTouch = sceneActive && sheet == nil && !preferences.editingPreview && !showsInGameGuide
        let usesDeviceMotion = config.motion == .ipad ||
            (config.motion == .automatic && !input.controllerConnected &&
             !input.suppressesAutomaticDeviceMotion)
        // Keep motion sampling alive while the Play sheet is presented. Stopping
        // it resets the fusion filter, makes the emulated pad fall to its default
        // attitude, then slowly converge again when the sheet closes.
        return PlayInputState(touch: acceptsTouch && showsTouch,
                              deviceMotion: sceneActive && usesDeviceMotion,
                              controllerMotion: sceneActive && config.motion != .off,
                              keepAwake: scenePhase == .active && config.keepAwake,
                              sensitivity: config.motionSensitivity)
    }

    @ViewBuilder
    private var keyboardTouchForwarder: some View {
        if softwareKeyboardActive {
            GeometryReader { geo in
                Color.clear
                    .contentShape(Rectangle())
                    .gesture(
                        DragGesture(minimumDistance: 0)
                            .onChanged { value in
                                guard geo.size.width > 0, geo.size.height > 0 else { return }
                                let nx = Float(value.location.x / geo.size.width)
                                let ny = Float(value.location.y / geo.size.height)
                                CemuBridge_UpdateMainTouch(true, nx, ny)
                            }
                            .onEnded { _ in
                                CemuBridge_UpdateMainTouch(false, 0, 0)
                                let updated = String(cString: CemuBridge_GetSoftwareKeyboardText())
                                if !updated.isEmpty && softwareKeyboardText != updated {
                                    softwareKeyboardText = updated
                                }
                            }
                    )
            }
            .ignoresSafeArea()
        }
    }

    @ViewBuilder
    private var touchSurfaceView: some View {
        if showsTouch && sheet == nil && !preferences.editingPreview && !showsInGameGuide && scenePhase == .active {
            TouchControllerSurface(session: input, layout: TouchLayout(rawValue: config.layout) ?? .centered,
                                   scale: config.scale, opacity: config.opacity, reach: config.reach,
                                   floatingSticks: config.floatingSticks, haptics: config.haptics,
                                   spacing: config.buttonSpacing, stickTravel: config.stickTravel,
                                   offsets: config.customPositions[config.layout] ?? [:],
                                   externalDisplayMode: externalDisplay.isConnected)
                .ignoresSafeArea()
                .transition(reduceMotion ? .opacity : .opacity.combined(with: .offset(y: 18)))
        }
    }

    @ViewBuilder
    private var overlayHeaderView: some View {
        if softwareKeyboardActive {
            SoftwareKeyboardInputBar(
                text: $softwareKeyboardText,
                maxLength: softwareKeyboardMaxLen,
                onConfirm: {
                    CemuBridge_SendSoftwareKeyboardString(softwareKeyboardText)
                    CemuBridge_ConfirmSoftwareKeyboard()
                },
                onBackspace: {
                    if !softwareKeyboardText.isEmpty {
                        softwareKeyboardText.removeLast()
                        CemuBridge_SendSoftwareKeyboardString(softwareKeyboardText)
                    } else {
                        CemuBridge_SendSoftwareKeyboardKey(8)
                    }
                },
                onSpace: {
                    if softwareKeyboardText.count < softwareKeyboardMaxLen {
                        softwareKeyboardText.append(" ")
                        CemuBridge_SendSoftwareKeyboardString(softwareKeyboardText)
                    }
                }
            )
            .padding(.horizontal, 16).padding(.top, 8)
            .transition(reduceMotion ? .opacity : .move(edge: .top).combined(with: .opacity))
        } else {
            PlayToolbar(
                showFPS: showFPS,
                editingPreview: preferences.editingPreview,
                gamePadOnly: config.display == .gamepad,
                lockedTriggers: input.lockedTriggers.map(\.rawValue).sorted(),
                showDebug: showDebugButton,
                shareFeedback: showsDebugFeedback,
                release: { input.releaseAll() },
                finishEditing: { preferences.editingPreview = false },
                swap: {
                    if externalDisplay.isConnected { sheet = .controls }
                    else {
                        input.releaseAll()
                        preferences.editingPreview = false
                        preferences.configuration.display = config.display == .gamepad ? .overlay : .gamepad
                    }
                },
                share: {
                    showsDebugFeedback = true
                    debugNotice = "Sharing diagnostics…"
                    UIImpactFeedbackGenerator(style: .medium).impactOccurred()
                    onShareDiagnostics()
                },
                diagnostics: { input.releaseAll(); showDiagnostics() },
                capture: {
                    input.releaseAll()
                    debugNotice = CemuBridge_RequestGPUFrameCapture()
                        ? "Graphics capture requested. Play may pause briefly."
                        : "Start a game before capturing a graphics frame."
                },
                controls: { preferences.editingPreview = false; sheet = .controls },
                exitGame: {
                    input.releaseAll()
                    showsExitConfirmation = true
                }
            )
            .padding(.horizontal, 16).padding(.top, 8)
        }
    }

    var body: some View {
        ZStack {
            keyboardTouchForwarder
            touchSurfaceView
            VStack {
                overlayHeaderView
                .task(id: showsDebugFeedback) {
                    guard showsDebugFeedback else { return }
                    do { try await Task.sleep(for: .milliseconds(1500)) } catch { return }
                    showsDebugFeedback = false
                }
                if let connectionNotice {
                    Label(connectionNotice, systemImage: input.controllerConnected ? "gamecontroller.fill" : "hand.draw")
                        .font(.system(size: 12, weight: .medium))
                        .foregroundStyle(.white.opacity(0.85))
                        .padding(.horizontal, 16).padding(.vertical, 10)
                        .modifier(LibraryGlassSurface(cornerRadius: 24))
                        .transition(.opacity.combined(with: .offset(y: reduceMotion ? 0 : -6)))
                        .allowsHitTesting(false)
                }
                if let debugNotice {
                    Label(debugNotice, systemImage: "checkmark.circle.fill")
                        .font(.system(size: 12, weight: .medium))
                        .foregroundStyle(.white.opacity(0.95))
                        .padding(.horizontal, 16).padding(.vertical, 10)
                        .modifier(LibraryGlassSurface(cornerRadius: 24))
                        .transition(.opacity.combined(with: .offset(y: reduceMotion ? 0 : -6)))
                        .allowsHitTesting(false)
                }
                Spacer()
            }

        }
        .animation(reduceMotion ? .easeOut(duration: 0.15) : .spring(response: 0.38, dampingFraction: 0.9), value: showsTouch)
        .animation(reduceMotion ? nil : .smooth(duration: 0.18), value: input.lockedTriggers)
        .animation(reduceMotion ? nil : .smooth(duration: 0.2), value: connectionNotice)
        .animation(reduceMotion ? nil : .smooth(duration: 0.2), value: debugNotice)
        .animation(reduceMotion ? .easeOut(duration: 0.15) : .spring(response: 0.34, dampingFraction: 0.9), value: showsInGameGuide)
        .task(id: connectionNotice) {
            guard connectionNotice != nil else { return }
            do { try await Task.sleep(nanoseconds: 2_200_000_000) } catch { return }
            connectionNotice = nil
        }
        .task(id: debugNotice) {
            guard debugNotice != nil else { return }
            do { try await Task.sleep(nanoseconds: 2_500_000_000) } catch { return }
            debugNotice = nil
        }
        .task(id: scenePhase) {
            guard scenePhase == .active else { return }
            while !Task.isCancelled {
                let active = CemuBridge_IsSoftwareKeyboardActive()
                if softwareKeyboardActive != active {
                    input.releaseAll()
                    softwareKeyboardActive = active
                    if active {
                        softwareKeyboardText = String(cString: CemuBridge_GetSoftwareKeyboardText())
                        softwareKeyboardMaxLen = max(1, Int(CemuBridge_GetSoftwareKeyboardMaxTextLength()))
                    }
                }
                do { try await Task.sleep(for: .milliseconds(active ? 80 : 160)) } catch { return }
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: Notification.Name("77U.ControllerMenu"))) { _ in
            input.releaseAll()
            sheet = .controls
        }
        .sheet(item: $sheet) { _ in
            PlaySettingsPanel(preferences: preferences, input: input, showGamePad: $showGamePad)
            .preferredColorScheme(.dark)
        }
        .sheet(isPresented: $showsInGameGuide) {
            InGameControlsGuide { hasSeenInGameGuide = true; showsInGameGuide = false }
                .modifier(MenuSheetPresentation())
                .interactiveDismissDisabled()
        }
        .onAppear {
            previousIdleTimerDisabled = UIApplication.shared.isIdleTimerDisabled
            input.refreshController()
            if !hasSeenInGameGuide { showsInGameGuide = true }
            synchronize(inputState)
        }
        .onDisappear {
            input.stop()
            UIApplication.shared.isIdleTimerDisabled = previousIdleTimerDisabled
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in
            input.refreshController()
            restoreControllerAudioRoute()
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in
            input.refreshController()
            restoreControllerAudioRoute()
            Task { @MainActor in
                try? await Task.sleep(for: .milliseconds(180))
                input.refreshController()
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidBecomeCurrent)) { _ in
            input.refreshController()
            restoreControllerAudioRoute()
        }
        .onReceive(NotificationCenter.default.publisher(for: AVAudioSession.routeChangeNotification)) { notification in
            guard let value = notification.userInfo?[AVAudioSessionRouteChangeReasonKey] as? NSNumber,
                  let reason = AVAudioSession.RouteChangeReason(rawValue: value.uintValue),
                  reason == .newDeviceAvailable || reason == .oldDeviceUnavailable ||
                  reason == .routeConfigurationChange else { return }
            restoreControllerAudioRoute()
        }
        .onChange(of: input.controllerConnected) { _, connected in
            connectionNotice = connected ? "Controller connected" : "Touch controls ready"
        }
        .onChange(of: externalDisplay.isConnected) {
            input.releaseAll()
            preferences.editingPreview = false
        }
        .onChange(of: inputState) { _, state in synchronize(state) }
        .onChange(of: sheet) { input.releaseAll() }
        .onChange(of: scenePhase) { input.releaseAll() }
        .onChange(of: config) { input.releaseAll() }
        .confirmationDialog("Exit Game?", isPresented: $showsExitConfirmation, titleVisibility: .visible) {
            Button("Exit Game", role: .destructive) {
                onExitGame()
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Are you sure you want to exit the current game and return to the main menu?")
        }
    }

    private func synchronize(_ state: PlayInputState) {
        input.setEnabled(state.touch)
        CemuBridge_SetControllerMotionEnabled(state.controllerMotion)
        input.updateMotion(enabled: state.deviceMotion, sensitivity: state.sensitivity)
        UIApplication.shared.isIdleTimerDisabled = state.keepAwake
    }

    private func restoreControllerAudioRoute() {
        controllerAudioRouteTask?.cancel()
        controllerAudioRouteTask = Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(280))
            guard !Task.isCancelled else { return }
            guard scenePhase == .active else { return }
            CemuBridge_RestoreControllerAudioRoute()
        }
    }


}

/// Native glass controls update only when their visible state changes.
struct PlayToolbar: View {
    let showFPS: Bool
    let editingPreview: Bool
    let gamePadOnly: Bool
    let lockedTriggers: [String]
    let showDebug: Bool
    let shareFeedback: Bool
    let release: () -> Void
    let finishEditing: () -> Void
    let swap: () -> Void
    let share: () -> Void
    let diagnostics: () -> Void
    let capture: () -> Void
    let controls: () -> Void
    let exitGame: () -> Void
    private var isPhone: Bool { UIDevice.current.userInterfaceIdiom == .phone }

    var body: some View {
        LibraryGlassGroup {
            HStack(spacing: 8) {
                if showFPS && !(isPhone && editingPreview) { PlayPerformanceBadge() }
                if !lockedTriggers.isEmpty {
                    Button(action: release) {
                        Label(lockedTriggers.joined(separator: " + "), systemImage: "lock.fill")
                            .font(.caption.weight(.semibold))
                    }
                    .modifier(CompactPlayToolbarButton())
                    .accessibilityLabel("Release locked triggers")
                }
                Spacer(minLength: 6)
                if editingPreview {
                    Button("Done", action: finishEditing).modifier(CompactPlayToolbarButton())
                }
                Button(action: swap) {
                    Image(systemName: gamePadOnly ? "tv" : "rectangle.on.rectangle")
                        .modifier(PlaySymbolTransition())
                        .frame(minWidth: 20, minHeight: 18)
                }
                .modifier(CompactPlayToolbarButton())
                .accessibilityLabel("Swap TV and GamePad")
                if showDebug && !editingPreview {
                    Button(action: share) {
                        Image(systemName: shareFeedback ? "checkmark" : "square.and.arrow.up")
                            .modifier(PlaySymbolTransition())
                            .frame(minWidth: 20, minHeight: 18)
                    }
                    .modifier(CompactPlayToolbarButton())
                    .accessibilityLabel("Share diagnostics")
                    .contextMenu {
                        Button("Open diagnostics", systemImage: "wrench.and.screwdriver", action: diagnostics)
                        if CemuBridge_IsGPUFrameCaptureSupported() {
                            Button("Capture graphics frame", systemImage: "camera.viewfinder", action: capture)
                        }
                    }
                }
                Button(action: controls) {
                    HStack(spacing: 7) {
                        Image(systemName: "slider.horizontal.3")
                        if !isPhone { Text("Controls") }
                    }.frame(minHeight: 18)
                }
                .modifier(CompactPlayToolbarButton())
                .accessibilityIdentifier("play-menu")
                .accessibilityLabel("Controls and layout settings")

                Button(action: exitGame) {
                    Image(systemName: "xmark.circle")
                        .modifier(PlaySymbolTransition())
                        .frame(minWidth: 20, minHeight: 18)
                }
                .modifier(CompactPlayToolbarButton())
                .accessibilityIdentifier("play-exit")
                .accessibilityLabel("Exit game")
            }
            .font(.subheadline)
            .buttonBorderShape(.capsule)
            .controlSize(.small)
        }
    }
}

/// Keep compact toolbar artwork inside a comfortable touch target.
private struct CompactPlayToolbarButton: ViewModifier {
    func body(content: Content) -> some View {
        content
            .modifier(NativeGlassButton())
            .frame(minWidth: 44, minHeight: 44)
            .contentShape(.rect)
    }
}

/// Only this small badge subscribes to performance updates. Touch controls stay idle.
private struct PlayPerformanceBadge: View {
    @State private var showsDetails = false
    @State private var fps = 0.0
    @State private var sourceFPS = 0.0
    @State private var gpu = 0.0
    @State private var wait = 0.0
    @State private var draws: UInt32 = 0
    @State private var passes: UInt32 = 0
    private let timer = Timer.publish(every: 0.3, on: .main, in: .common).autoconnect()
    private var frameRate: Int { Int(max(0, fps.isFinite ? fps.rounded() : 0)) }
    private var sourceFrameRate: Int { Int(max(0, sourceFPS.isFinite ? sourceFPS.rounded() : 0)) }
    private var rateLabel: String { sourceFrameRate > 0 ? "\(sourceFrameRate) -> \(frameRate)" : "\(frameRate)" }

    var body: some View {
        Button { showsDetails.toggle() } label: {
            HStack(spacing: 5) {
                Text(rateLabel).monospacedDigit().fontWeight(.semibold)
                Text("fps").foregroundStyle(.secondary)
            }
            .font(.caption)
            .padding(.horizontal, 2).frame(minHeight: 18)
        }
        .modifier(CompactPlayToolbarButton())
        .accessibilityLabel("Performance")
        .accessibilityValue(sourceFrameRate > 0 ? "Game \(sourceFrameRate), output \(frameRate) frames per second" : "\(frameRate) frames per second")
        .popover(isPresented: $showsDetails) {
            VStack(spacing: 12) {
                if sourceFrameRate > 0 {
                    LabeledContent("Game frame rate", value: "\(sourceFrameRate) fps")
                    LabeledContent("Display output", value: "\(frameRate) fps")
                } else {
                    LabeledContent("Frame rate", value: "\(frameRate) fps")
                }
                LabeledContent("GPU time", value: String(format: "%.1f ms", gpu))
                LabeledContent("CPU wait", value: String(format: "%.1f ms", wait))
                LabeledContent("Draw calls", value: String(draws))
                LabeledContent("Render passes", value: String(passes))
            }
            .font(.subheadline).monospacedDigit().padding(20).frame(minWidth: 240)
            .presentationCompactAdaptation(.popover)
        }
        .onReceive(timer) { _ in
            let sample = CemuBridge_GetPerfStatsSnapshot()
            let frameGeneration = CemuBridge_GetFrameGenerationRates()
            sourceFPS = frameGeneration.sourceFPS
            fps = sourceFPS > 0 ? frameGeneration.displayedFPS : (sample.fps > 0 ? sample.fps : CemuBridge_GetCurrentFPS())
            if showsDetails {
                gpu = sample.gpuMs; wait = sample.cpuWaitMs
                draws = sample.drawCalls; passes = sample.renderPasses
            }
        }
    }
}

private struct InGameControlsGuide: View {
    let finish: () -> Void
    var body: some View {
        NavigationStack {
            List {
                Section {
                    Label("Touch controls appear when no controller is connected.", systemImage: "hand.tap")
                    Label("Swipe ZL or ZR to hold it. Tap again to release.", systemImage: "lock.open")
                    Label("Connect a controller through Bluetooth or USB.", systemImage: "gamecontroller")
                } header: { Text("Controls") }
                Section {
                    Label("Open Controls to move buttons, adjust motion or change screens.", systemImage: "slider.horizontal.3")
                    Label("Tap the screen button to swap TV and GamePad.", systemImage: "rectangle.on.rectangle")
                } header: { Text("During play") }
            }
            .navigationTitle("Ready to play")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Play", systemImage: "play.fill", action: finish)
                        .modifier(NativeGlassButton(prominent: true))
                }
            }
        }
    }
}

struct PlaySettingsPanel: View {
    @ObservedObject var preferences: PlayPreferences
    @ObservedObject var input: TouchInputSession
    @Binding var showGamePad: Bool
    @Environment(\.dismiss) private var dismiss
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var showResetConfirmation = false
    @State private var editingLayout: TouchLayout?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Label(input.controllerConnected ? "Controller connected" : "Touch controls", systemImage: input.controllerConnected ? "gamecontroller.fill" : "hand.tap")
                    Button("Edit button positions", systemImage: "arrow.up.and.down.and.arrow.left.and.right") {
                        input.releaseAll(); editingLayout = .centered
                    }
                    .accessibilityIdentifier("play.editLayout")
                } header: { Text("Controller") }
                Section {
                    Picker("Show controls", selection: $preferences.configuration.visibility) {
                        Text("Automatic").tag(TouchVisibility.automatic)
                        Text("Always").tag(TouchVisibility.always)
                        Text("Hidden").tag(TouchVisibility.hidden)
                    }
                    slider("Size", value: $preferences.configuration.scale, range: 0.75...1.25,
                           label: "\(Int(preferences.configuration.scale * 100))%")
                    slider("Opacity", value: $preferences.configuration.opacity, range: 0.25...1,
                           label: "\(Int(preferences.configuration.opacity * 100))%")
                    slider("Edge inset", value: $preferences.configuration.reach, range: 0...1,
                           label: preferences.configuration.reach < 0.25 ? "Wide" : "Closer")
                    slider("Button spacing", value: $preferences.configuration.buttonSpacing, range: 0.9...1.3,
                           label: "\(Int(preferences.configuration.buttonSpacing * 100))%")
                    slider("Stick travel", value: $preferences.configuration.stickTravel, range: 40...120,
                           label: "\(Int(preferences.configuration.stickTravel)) pt")
                    Toggle("Floating sticks", isOn: $preferences.configuration.floatingSticks).controllerToggle($preferences.configuration.floatingSticks)
                    Toggle("Haptics", isOn: $preferences.configuration.haptics).controllerToggle($preferences.configuration.haptics)
                    Button("Release held buttons", systemImage: "hand.raised") { input.releaseAll() }
                } header: { Text("Touch") } footer: {
                    Text("Automatic hides touch controls while a controller is connected. Floating sticks start where your thumb lands.")
                }
                Section {
                    Picker("Source", selection: $preferences.configuration.motion) {
                        Text("Off").tag(PlayMotionMode.off)
                        Text("Automatic").tag(PlayMotionMode.automatic)
                        Text("This device").tag(PlayMotionMode.ipad)
                    }
                    slider("Sensitivity", value: $preferences.configuration.motionSensitivity, range: 0.5...2,
                           label: String(format: "%.1f×", preferences.configuration.motionSensitivity))
                    Button("Recenter", systemImage: "scope") { input.recenter() }
                } header: { Text("Motion") } footer: {
                    Text("Automatic uses controller motion when available. Hold the device comfortably before recentering.")
                }
                Section {
                    ExternalDisplaySettingsView()
                    Picker("Layout", selection: $preferences.configuration.display) {
                        Text("Picture in picture").tag(PlayDisplayMode.overlay)
                        Text("Split screen").tag(PlayDisplayMode.split)
                        Text("GamePad only").tag(PlayDisplayMode.gamepad)
                    }.disabled(ExternalDisplayCoordinator.shared.isConnected)
                    Toggle("Show GamePad preview", isOn: $showGamePad).controllerToggle($showGamePad)
                    if preferences.configuration.display != .split {
                        Picker("Preview position", selection: $preferences.configuration.previewPosition) {
                            Text("Top center").tag(PadPreviewPosition.top)
                            Text("Top left").tag(PadPreviewPosition.left)
                            Text("Top right").tag(PadPreviewPosition.right)
                            Text("Bottom center").tag(PadPreviewPosition.bottom)
                        }.onChange(of: preferences.configuration.previewPosition) { preferences.configuration.previewAnchor = nil }
                        Button("Move preview", systemImage: "arrow.up.and.down.and.arrow.left.and.right") {
                            showGamePad = true; preferences.editingPreview = true; dismiss()
                        }
                        slider("Preview size", value: $preferences.configuration.previewScale, range: 0.7...1.4,
                               label: "\(Int(preferences.configuration.previewScale * 100))%")
                    }
                    Toggle("Native display resolution", isOn: $preferences.configuration.nativeDisplayResolution).controllerToggle($preferences.configuration.nativeDisplayResolution)
                    Toggle("Keep screen awake", isOn: $preferences.configuration.keepAwake).controllerToggle($preferences.configuration.keepAwake)
                } header: { Text("Screens") } footer: {
                    Text("Turn off native display resolution to reduce GPU work. Preview position and size are saved.")
                }
                Section {
                    Button("Reset control settings", role: .destructive) { showResetConfirmation = true }
                } footer: { Text("The game keeps running. Pause in the game before adjusting controls.") }
            }
            .navigationTitle("Controls")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
            .animation(reduceMotion ? nil : .smooth(duration: 0.2), value: preferences.configuration.display)
            .confirmationDialog("Reset controls, motion and screen layout?", isPresented: $showResetConfirmation,
                                titleVisibility: .visible) {
                Button("Reset settings", role: .destructive) { preferences.configuration = PlayConfiguration() }
                Button("Cancel", role: .cancel) {}
            }
        }
        .modifier(MenuSheetPresentation())
        .fullScreenCover(item: $editingLayout) { layout in
            TouchLayoutEditor(preferences: preferences, layout: layout)
        }
    }

    private func slider(_ title: String, value: Binding<Double>, range: ClosedRange<Double>, label: String) -> some View {
        VStack(spacing: 8) {
            HStack { Text(title); Spacer(); Text(label).monospacedDigit().foregroundStyle(.secondary) }
            Slider(value: value, in: range).controllerAdjustable(value, range: range).accessibilityLabel(title)
        }.padding(.vertical, 4)
    }
}

private struct PlaySymbolTransition: ViewModifier {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @ViewBuilder func body(content: Content) -> some View {
        if #available(iOS 17.0, *), !reduceMotion {
            content.contentTransition(.symbolEffect(.replace))
        } else { content }
    }
}

struct SoftwareKeyboardInputBar: View {
    @Binding var text: String
    let maxLength: Int
    let onConfirm: () -> Void
    let onBackspace: () -> Void
    let onSpace: () -> Void
    @FocusState private var isFieldFocused: Bool
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        VStack(spacing: 8) {
            HStack(spacing: 8) {
                Image(systemName: "keyboard.fill")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(Color.accentColor)

                Text("Tastatureingabe")
                    .font(.system(size: 14, weight: .bold))
                    .foregroundStyle(.white)

                Spacer()

                Text("\(text.count)/\(maxLength)")
                    .font(.system(size: 12, weight: .medium, design: .monospaced))
                    .foregroundStyle(.white.opacity(0.7))
            }

            HStack(spacing: 8) {
                HStack(spacing: 8) {
                    Image(systemName: "pencil")
                        .foregroundStyle(.white.opacity(0.5))
                        .font(.system(size: 14))

                    TextField("Namen eingeben…", text: $text)
                        .font(.system(size: 16, weight: .semibold))
                        .foregroundStyle(.white)
                        .focused($isFieldFocused)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                        .submitLabel(.done)
                        .onSubmit {
                            onConfirm()
                        }
                        .onChange(of: text) { _, newValue in
                            if newValue.count > maxLength {
                                text = String(newValue.prefix(maxLength))
                            }
                            CemuBridge_SendSoftwareKeyboardString(text)
                        }

                    if !text.isEmpty {
                        Button(action: {
                            text = ""
                            CemuBridge_SendSoftwareKeyboardString("")
                        }) {
                            Image(systemName: "xmark.circle.fill")
                                .foregroundStyle(.white.opacity(0.6))
                        }
                    }
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 7)
                .background(Color.white.opacity(0.12), in: RoundedRectangle(cornerRadius: 10))

                Button(action: onSpace) {
                    Text("␣ Space")
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 7)
                        .background(Color.white.opacity(0.15), in: RoundedRectangle(cornerRadius: 10))
                }

                Button(action: onBackspace) {
                    Image(systemName: "delete.left.fill")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 7)
                        .background(Color.white.opacity(0.15), in: RoundedRectangle(cornerRadius: 10))
                }

                Button(action: onConfirm) {
                    HStack(spacing: 5) {
                        Image(systemName: "checkmark")
                            .font(.system(size: 13, weight: .black))
                        Text("Confirm")
                            .font(.system(size: 14, weight: .bold))
                    }
                    .foregroundStyle(.white)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 7)
                    .background(Color.green, in: RoundedRectangle(cornerRadius: 10))
                    .shadow(color: .green.opacity(0.4), radius: 6, x: 0, y: 2)
                }
            }
        }
        .padding(10)
        .modifier(LibraryGlassSurface(cornerRadius: 16))
        .onAppear {
            isFieldFocused = true
        }
    }
}

