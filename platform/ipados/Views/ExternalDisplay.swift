import SwiftUI
import UIKit

/// Moves one renderer-owned TV surface between the iPad and an external display.
/// Keeping the CAMetalLayer avoids copying every TV frame and saves GPU bandwidth.
@MainActor
final class ExternalDisplayCoordinator: ObservableObject {
    static let shared = ExternalDisplayCoordinator()

    @Published private(set) var isConnected = false
    @Published var isGameRunning = false
    let preferences = PlayPreferences()

    fileprivate let tvView = EmulatorMetalView(frame: .zero)
    private weak var localHost: TVSurfaceHost?
    private weak var externalHost: TVSurfaceHost?

    private init() {
        tvView.rendersDRC = false
    }

    fileprivate func register(_ host: TVSurfaceHost, role: TVSurfaceRole) {
        switch role {
        case .local: localHost = host
        case .external: externalHost = host
        }
        routeSurface()
    }

    fileprivate func unregister(_ host: TVSurfaceHost, role: TVSurfaceRole) {
        switch role {
        case .local where localHost === host: localHost = nil
        case .external where externalHost === host: externalHost = nil
        default: break
        }
        routeSurface()
    }

    fileprivate func update(nativeResolution: Bool, onLayerAttached: ((Bool) -> Void)?) {
        tvView.nativeResolution = nativeResolution
        if let onLayerAttached { tvView.onLayerAttachedChange = onLayerAttached }
    }

    private func usableExternalHost() -> TVSurfaceHost? {
        guard let host = externalHost, let window = host.window,
              !window.isHidden, window.windowScene?.activationState != .unattached else { return nil }
        return host
    }

    fileprivate func externalSceneDidDisconnect(_ window: UIWindow?) {
        // iPadOS only owns one non-interactive external scene for this app. The
        // host can already have lost its UIWindow by the time this callback runs.
        externalHost = nil
        // UIKit can leave the detached host's window reachable until the next
        // run loop. Route explicitly so the iPad TV surface is restored now.
        routeSurface(forceLocal: true)
    }

    fileprivate func externalSceneDidConnect() {
        routeSurface()
    }

    private func routeSurface(forceLocal: Bool = false) {
        let external = forceLocal ? nil : usableExternalHost()
        let target = external ?? localHost
        let connected = external != nil
        if isConnected != connected {
            isConnected = connected
            NotificationCenter.default.post(name: .cemuExternalDisplayChanged, object: connected)
        }
        guard let target else { return }
        if tvView.superview !== target {
            tvView.transferringBetweenWindows = tvView.window != nil && target.window != nil
            tvView.removeFromSuperview()
            tvView.frame = target.bounds
            tvView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            target.addSubview(tvView)
            tvView.transferringBetweenWindows = false
            tvView.setNeedsLayout()
            tvView.layoutIfNeeded()
        }
    }
}

extension Notification.Name {
    static let cemuExternalDisplayChanged = Notification.Name("CemuExternalDisplayChanged")
}

fileprivate enum TVSurfaceRole { case local, external }

@MainActor
fileprivate final class TVSurfaceHost: UIView {
    let role: TVSurfaceRole
    init(role: TVSurfaceRole) {
        self.role = role
        super.init(frame: .zero)
        backgroundColor = .black
        isUserInteractionEnabled = false
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil { ExternalDisplayCoordinator.shared.register(self, role: role) }
        else { ExternalDisplayCoordinator.shared.unregister(self, role: role) }
    }
    override func layoutSubviews() {
        super.layoutSubviews()
        if ExternalDisplayCoordinator.shared.tvView.superview === self {
            ExternalDisplayCoordinator.shared.tvView.frame = bounds
        }
    }
}

struct RoutedTVView: UIViewRepresentable {
    var nativeResolution: Bool
    var onLayerAttached: ((Bool) -> Void)?
    func makeUIView(context: Context) -> UIView { TVSurfaceHost(role: .local) }
    func updateUIView(_ view: UIView, context: Context) {
        ExternalDisplayCoordinator.shared.update(nativeResolution: nativeResolution,
                                                  onLayerAttached: onLayerAttached)
    }
}

private struct ExternalTVHostView: UIViewRepresentable {
    func makeUIView(context: Context) -> UIView { TVSurfaceHost(role: .external) }
    func updateUIView(_ view: UIView, context: Context) {
        ExternalDisplayCoordinator.shared.update(
            nativeResolution: ExternalDisplayCoordinator.shared.preferences.configuration.nativeDisplayResolution,
            onLayerAttached: nil)
    }
}

@MainActor
final class CemuApplicationDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     supportedInterfaceOrientationsFor window: UIWindow?) -> UIInterfaceOrientationMask {
        GameOrientationCoordinator.supportedOrientations
    }

    func application(_ application: UIApplication,
                     configurationForConnecting connectingSceneSession: UISceneSession,
                     options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        if connectingSceneSession.role == .windowExternalDisplayNonInteractive {
            let configuration = UISceneConfiguration(name: "77U External Display",
                                                     sessionRole: connectingSceneSession.role)
            configuration.delegateClass = ExternalDisplaySceneDelegate.self
            return configuration
        }
        return UISceneConfiguration(name: "Default Configuration",
                                    sessionRole: connectingSceneSession.role)
    }
}

@MainActor
final class ExternalDisplaySceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?

    func scene(_ scene: UIScene, willConnectTo session: UISceneSession,
               options connectionOptions: UIScene.ConnectionOptions) {
        guard session.role == .windowExternalDisplayNonInteractive,
              let windowScene = scene as? UIWindowScene else { return }
        let window = UIWindow(windowScene: windowScene)
        window.backgroundColor = .black
        window.rootViewController = UIHostingController(rootView: ExternalDisplayRootView()
            .preferredColorScheme(.dark))
        self.window = window
        window.isHidden = false
        // Hosting the SwiftUI root can register its surface before the external
        // UIWindow becomes visible. Re-evaluate after visibility changes too.
        ExternalDisplayCoordinator.shared.externalSceneDidConnect()
        DispatchQueue.main.async { ExternalDisplayCoordinator.shared.externalSceneDidConnect() }
    }

    func sceneDidDisconnect(_ scene: UIScene) {
        ExternalDisplayCoordinator.shared.externalSceneDidDisconnect(window)
        window?.isHidden = true
        window = nil
    }
}

private struct ExternalDisplayRootView: View {
    @ObservedObject private var coordinator = ExternalDisplayCoordinator.shared
    @ObservedObject private var preferences = ExternalDisplayCoordinator.shared.preferences

    var body: some View {
        GeometryReader { proxy in
            let both = preferences.configuration.externalDisplayLayout == .tvAndGamePad
            ZStack {
                Color.black.ignoresSafeArea()
                if both {
                    HStack(spacing: 18) {
                        ExternalTVHostView()
                            .aspectRatio(16 / 9, contentMode: .fit)
                            .frame(width: proxy.size.width * 0.66)
                            .clipShape(.rect(cornerRadius: 12))
                        DRCPreviewView(isActive: coordinator.isGameRunning,
                                       nativeResolution: preferences.configuration.nativeDisplayResolution)
                            .aspectRatio(854 / 480, contentMode: .fit)
                            .frame(width: proxy.size.width * 0.27)
                            .clipShape(.rect(cornerRadius: 12))
                            .overlay { RoundedRectangle(cornerRadius: 12).stroke(.white.opacity(0.2)) }
                            .allowsHitTesting(false)
                    }
                    .padding(max(18, min(proxy.size.width, proxy.size.height) * 0.035))
                } else {
                    ExternalTVHostView().ignoresSafeArea()
                }
                if !coordinator.isGameRunning {
                    VStack(spacing: 14) {
                        Image(systemName: "display").font(.system(size: 42, weight: .light))
                        Text("77U").font(.title.bold())
                        Text("Choose a game on your iPad").foregroundStyle(.secondary)
                    }
                    .padding(30).background(.ultraThinMaterial, in: .rect(cornerRadius: 24))
                }
            }
        }
    }
}

struct ExternalDisplaySettingsView: View {
    @ObservedObject private var coordinator = ExternalDisplayCoordinator.shared
    @ObservedObject private var preferences = ExternalDisplayCoordinator.shared.preferences
    var body: some View {
        Group {
            LabeledContent("External display", value: coordinator.isConnected ? "Connected" : "Not connected")
            Picker("TV layout", selection: $preferences.configuration.externalDisplayLayout) {
                Text("TV only").tag(ExternalDisplayLayout.tvOnly)
                Text("TV and GamePad").tag(ExternalDisplayLayout.tvAndGamePad)
            }
            Toggle("Keep touch controls on this device", isOn: $preferences.configuration.externalTouchControls).controllerToggle($preferences.configuration.externalTouchControls)
            Text("Connect a display by cable or use Screen Mirroring. The TV moves to the display; this device remains the GamePad.")
                .font(.footnote).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}
