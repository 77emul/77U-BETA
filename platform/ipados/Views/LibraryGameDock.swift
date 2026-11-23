import SwiftUI

enum LibraryLaunchMode: Hashable {
    case withJIT
    case standardNoJIT

    init(cpuMode: Int) {
        self = cpuMode == 1 ? .standardNoJIT : .withJIT
    }

    func apply() {
        switch self {
        case .withJIT:
            if CemuBridge_GetCPUDiagnosticMode() == 1 {
                CemuBridge_SetCPUDiagnosticMode(2)
            }
        case .standardNoJIT:
            CemuBridge_SetCPUDiagnosticMode(1)
        }
    }
}

struct LibraryGameDock: View {
    @ObservedObject private var theme = MenuThemeStore.shared
    let game: GameLibraryEntry
    let compact: Bool
    var inset: CGFloat = 18
    let play: (GameLibraryEntry) -> Void
    let manageSaves: (GameLibraryEntry) -> Void
    let manageGraphics: (GameLibraryEntry) -> Void
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @Environment(\.colorScheme) private var colorScheme
    @AppStorage("CemuIOS.CPUDiagnosticMode") private var cpuMode = 2

    private var launchMode: LibraryLaunchMode {
        LibraryLaunchMode(cpuMode: cpuMode)
    }

    private var launchModeSelection: Binding<LibraryLaunchMode> {
        Binding(get: { launchMode }, set: { $0.apply() })
    }

    var body: some View {
        VStack(spacing: compact ? 8 : 12) {
            VStack(spacing: 4) {
                Text(game.title).font(compact ? .headline : .title3.weight(.semibold))
                    .multilineTextAlignment(.center).lineLimit(2)
                    .contentTransition(.opacity)
                HStack(spacing: 6) {
                    if let region = game.region { Text(region) }
                    if let version = game.version { Text("v\(version)") }
                    if game.hasUpdate { Image(systemName: "checkmark.seal.fill").accessibilityLabel("Update installed") }
                }
                .font(.caption)
            }
            HStack(spacing: 6) {
                Button { manageSaves(game) } label: { dockIcon("externaldrive") }
                    .controllerButtonStyle(.plain)
                    .accessibilityLabel("Saves").accessibilityIdentifier("library.saves")
                    .disabled(game.numericTitleID == nil)
                Button { play(game) } label: {
                    let config = theme.configuration
                    Label(config.enabled ? config.playTitle : "Play",
                          systemImage: config.enabled ? config.playIcon : "play.fill")
                        .font(.headline).lineLimit(1).minimumScaleFactor(0.7)
                        .padding(.horizontal, 12)
                        .frame(minWidth: 82, idealWidth: 114, maxWidth: 138, minHeight: 44)
                        .foregroundStyle(config.enabled ? config.playInk : Color.black)
                        .background(config.enabled ? Color(themeHex: config.playColor) : .white,
                                    in: RoundedRectangle(cornerRadius: config.enabled ? config.cornerRadius : 24))
                        .contentShape(Capsule())
                }
                .controllerButtonStyle(.plain)
                .accessibilityIdentifier("library.play")
                Menu {
                    Picker("Launch mode", selection: launchModeSelection) {
                        Text("With JIT").tag(LibraryLaunchMode.withJIT)
                        Text("Without JIT (standard)").tag(LibraryLaunchMode.standardNoJIT)
                    }
                    Divider()
                    Button("Graphics & mods", systemImage: "slider.horizontal.3") { manageGraphics(game) }
                        .disabled(game.numericTitleID == nil)
                } label: { dockIcon("slider.horizontal.3") }
                .controllerButtonStyle(.plain)
                .accessibilityLabel("Launch mode, graphics and mods")
                .accessibilityIdentifier("library.graphics")
            }
            .padding(5)
            .background {
                if reduceTransparency {
                    Capsule().fill(colorScheme == .light ? Color(uiColor: .systemGray5) : .black)
                } else {
                    Capsule().fill(.regularMaterial)
                        .overlay { Capsule().fill(colorScheme == .light ? .white.opacity(0.38) : .black.opacity(0.18)) }
                }
            }
            .overlay {
                Capsule().strokeBorder(colorScheme == .light ? .black.opacity(0.13) : .white.opacity(0.22))
                    .allowsHitTesting(false)
            }
            .frame(maxWidth: 238)
            if launchMode != .withJIT {
                Text("Without JIT · standard")
                    .font(.caption2).foregroundStyle(.secondary)
                    .accessibilityIdentifier("library.noJITWarning")
            }
        }
        .padding(.horizontal, inset)
        .frame(maxWidth: .infinity)
        .animation(reduceMotion ? nil : .smooth(duration: 0.2), value: game.id)
    }

    private func dockIcon(_ name: String) -> some View {
        Image(systemName: name)
            .font(.system(size: 18, weight: .semibold))
            .foregroundStyle(colorScheme == .light ? Color.black : .white)
            .frame(width: 44, height: 44)
            .background(colorScheme == .light ? .black.opacity(0.07) : .white.opacity(0.13), in: Circle())
            .contentShape(Circle())
    }

}
