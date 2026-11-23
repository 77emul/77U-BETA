import SwiftUI
import UIKit

@MainActor
enum GameOrientationCoordinator {
    static private(set) var isGameActive = false
    static private(set) var lockedGameOrientation: UIInterfaceOrientationMask = .landscapeRight
    static private(set) var isPortraitBlocked = false

    static var supportedOrientations: UIInterfaceOrientationMask {
        if UIDevice.current.userInterfaceIdiom == .phone {
            return .allButUpsideDown
        }
        return isGameActive ? lockedGameOrientation : .all
    }

    static func setGameActive(_ active: Bool) {
        if active {
            AppStartupSound.shared.stop()
            MenuThemeStore.shared.setMenuVisible(false)
        } else { MenuThemeStore.shared.setMenuVisible(true) }
        let wasActive = isGameActive
        if active && !wasActive {
            lockedGameOrientation = currentLandscapeOrientation()
        }
        isGameActive = active
        updateSuspension()
        apply(active)
        if active {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) {
                guard isGameActive else { return }
                apply(true)
            }
        }
    }

    static func reapplyIfNeeded() {
        guard isGameActive else { return }
        apply(true)
    }

    static func setPortraitBlocked(_ blocked: Bool) {
        // On iPhone, portrait dual-deck mode is fully supported and never blocked.
        let effectiveBlocked = (UIDevice.current.userInterfaceIdiom == .phone) ? false : blocked
        guard isPortraitBlocked != effectiveBlocked else { return }
        isPortraitBlocked = effectiveBlocked
        updateSuspension()
    }

    private static func updateSuspension() {
        let isBackgrounded = UIApplication.shared.applicationState != .active
        CemuBridge_SetAppSuspended(isGameActive && (isPortraitBlocked || isBackgrounded))
    }

    private static func apply(_ active: Bool) {
        guard let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .first(where: {
                $0.session.role == .windowApplication &&
                ($0.activationState == .foregroundActive || $0.activationState == .foregroundInactive)
            }) else { return }
        let isPhone = UIDevice.current.userInterfaceIdiom == .phone
        let orientations: UIInterfaceOrientationMask = isPhone ? .allButUpsideDown : (active ? lockedGameOrientation : .all)
        let rootController = scene.windows.first(where: \.isKeyWindow)?.rootViewController
            ?? scene.windows.first?.rootViewController
        if !isPhone {
            // iPadOS can resize a scene independently of its orientation. Keep
            // the play surface large enough for controls; ContentView blocks
            // gameplay if the system still presents a portrait-sized window.
            scene.sizeRestrictions?.minimumSize = active
                ? CGSize(width: 640, height: 360) : .zero
        }
        rootController?.setNeedsUpdateOfSupportedInterfaceOrientations()
        scene.requestGeometryUpdate(.iOS(interfaceOrientations: orientations),
                                    errorHandler: logOrientationRequestError)
    }

    // UIKit can deliver a rejected geometry request off the main executor.
    // This callback only calls the bridge's mutex-protected logger; keeping it
    // nonisolated avoids a Swift 6 actor assertion before the error is logged.
    nonisolated private static func logOrientationRequestError(_ error: any Error) {
        CemuBridge_LogJITMilestone("[UI] Orientation request failed: \(error.localizedDescription)")
    }

    private static func currentLandscapeOrientation() -> UIInterfaceOrientationMask {
        let orientation = UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .first(where: {
                $0.session.role == .windowApplication &&
                ($0.activationState == .foregroundActive || $0.activationState == .foregroundInactive)
            })?.effectiveGeometry.interfaceOrientation
        switch orientation {
        case .landscapeLeft: return .landscapeLeft
        case .landscapeRight: return .landscapeRight
        default: return .landscapeRight
        }
    }
}

@main
struct CemuIOSApp: App {
    @StateObject private var menuTheme = MenuThemeStore.shared
    @UIApplicationDelegateAdaptor(CemuApplicationDelegate.self) private var appDelegate
    @Environment(\.scenePhase) private var scenePhase

    init() {
        Self.applySharedRAMDefault()
        let success = CemuBridge_Initialize()
        if !success {
            print("[CemuIOS] WARNING: CemuBridge_Initialize() failed")
        }
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .background { MenuControllerNavigation().frame(width: 0, height: 0) }
                .tint(menuTheme.configuration.enabled ? Color(themeHex: menuTheme.configuration.accent) : .accentColor)
                .environment(\.locale, Locale(identifier: "en"))
                .onReceive(NotificationCenter.default.publisher(for: UIAccessibility.reduceMotionStatusDidChangeNotification)) { _ in
                    menuTheme.refreshAccessibility()
                }
                .onOpenURL { JITCoordinator.shared.handleVPNReturn($0) }
                .task {
                    await AppStartupSound.shared.playOnEntry()
                    if ProcessInfo.processInfo.arguments.contains("--jit-activation-self-test") {
                        await JITCoordinator.shared.runActivationSelfTest()
                    } else if ProcessInfo.processInfo.arguments.contains("--jit-helper-self-test") {
                        await JITCoordinator.shared.runHelperSelfTest()
                    }
                }
                .onReceive(NotificationCenter.default.publisher(for: UIApplication.didReceiveMemoryWarningNotification)) { _ in
                    CemuBridge_HandleMemoryWarning()
                }
        }
        .onChange(of: scenePhase) { _, newPhase in
            let pid = getpid()
            switch newPhase {
            case .active:
                CemuBridge_SetAppSuspended(GameOrientationCoordinator.isGameActive &&
                                            GameOrientationCoordinator.isPortraitBlocked)
                CemuBridge_RestoreAudioOutput()
                menuTheme.setActive(true)
                JITCoordinator.shared.applicationBecameActive()
                GameOrientationCoordinator.reapplyIfNeeded()
                CemuBridge_LogJITMilestone("[JIT] App resumed (scenePhase active, pid=\(pid))")
            case .inactive:
                menuTheme.setActive(false)
                if GameOrientationCoordinator.isGameActive {
                    CemuBridge_SetAppSuspended(true)
                }
                if GameOrientationCoordinator.isGameActive,
                   keepGameReadyInBackground {
                    CemuBridge_SetAppAudioMuted(true)
                }
                CemuBridge_LogJITMilestone("[JIT] App transitioned (scenePhase inactive, pid=\(pid))")
            case .background:
                menuTheme.setActive(false)
                if GameOrientationCoordinator.isGameActive {
                    CemuBridge_SetAppSuspended(true)
                }
                if GameOrientationCoordinator.isGameActive,
                   keepGameReadyInBackground {
                    CemuBridge_SetAppAudioMuted(true)
                }
                CemuBridge_LogJITMilestone("[JIT] App entered background (scenePhase background, pid=\(pid))")
            @unknown default:
                break
            }
        }
    }

    // Shared game RAM is on by default. Existing installs are switched on once;
    // after that the user's own choice in Settings is kept.
    private static func applySharedRAMDefault() {
        let defaults = UserDefaults.standard
        let migrationKey = "CemuIOS.SharedRAMDefaultOnApplied"
        guard !defaults.bool(forKey: migrationKey) else { return }
        defaults.set(true, forKey: "CemuIOS.ExperimentalSharedRAM")
        defaults.set(true, forKey: migrationKey)
    }

    private var keepGameReadyInBackground: Bool {
        let defaults = UserDefaults.standard
        return defaults.object(forKey: "CemuIOS.KeepGameReadyInBackground") == nil ||
            defaults.bool(forKey: "CemuIOS.KeepGameReadyInBackground")
    }
}
