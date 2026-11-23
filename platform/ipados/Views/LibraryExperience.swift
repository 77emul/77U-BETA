import SwiftUI
import UIKit

@MainActor
enum LibraryTheme {
    static var accent: Color { MenuThemeStore.shared.configuration.enabled ? Color(themeHex: MenuThemeStore.shared.configuration.accent) : .accentColor }
    static func selection(for scheme: ColorScheme) -> Color {
        let config = MenuThemeStore.shared.configuration
        if config.hasMedia && scheme == .light {
            return MenuThemeConfiguration.legibleColor(config.enabled ? config.accent : "0061AC",
                                                        over: config.libraryBackground(for: scheme))
        }
        return accent
    }
    static func background(for scheme: ColorScheme) -> Color { Color(uiColor: .systemBackground) }
    static func text(for scheme: ColorScheme) -> Color { MenuThemeStore.shared.configuration.libraryInk(for: scheme) }
}

/// System button styles provide the interaction and accessibility behavior.
struct NativeGlassButton: ViewModifier {
    var prominent = false
    @ViewBuilder func body(content: Content) -> some View {
        if #available(iOS 26, *) {
            if prominent { content.controllerButtonStyle(.glassProminent) }
            else { content.controllerButtonStyle(.glass) }
        } else {
            if prominent { content.controllerButtonStyle(.borderedProminent) }
            else { content.controllerButtonStyle(.bordered) }
        }
    }
}

/// Menus open below full height so the library or game stays in view.
struct MenuSheetPresentation: ViewModifier {
    var defaultColorScheme: ColorScheme?
    @ObservedObject private var theme = MenuThemeStore.shared
    @Environment(\.verticalSizeClass) private var verticalSizeClass

    func body(content: Content) -> some View {
        let phone = UIDevice.current.userInterfaceIdiom == .phone
        let landscapePhone = phone && verticalSizeClass == .compact
        sized(content, phone: phone)
            .modifier(MenuThemeAppearance())
            .preferredColorScheme(theme.configuration.enabled ? theme.configuration.surfaceScheme : defaultColorScheme)
            .background { if phone { PhoneMenuSheetWidth().frame(width: 0, height: 0) } }
            .presentationDetents([.fraction(landscapePhone ? 0.92 : phone ? 0.88 : 0.78), .large])
            .presentationBackground(theme.configuration.menuCanvas)
            .presentationDragIndicator(.visible)
            .presentationCornerRadius(phone ? (landscapePhone ? 32 : 40) : 28)
            .presentationCompactAdaptation(.sheet)
    }

    @ViewBuilder private func sized(_ content: Content, phone: Bool) -> some View {
        if phone { content.presentationSizing(.automatic) }
        else { content.presentationSizing(.page) }
    }
}

private struct PhoneMenuSheetWidth: UIViewControllerRepresentable {
    func makeUIViewController(context: Context) -> Controller { Controller() }
    func updateUIViewController(_ controller: Controller, context: Context) { controller.configure() }

    final class Controller: UIViewController {
        override func viewDidAppear(_ animated: Bool) {
            super.viewDidAppear(animated)
            configure()
        }
        override func viewDidLayoutSubviews() {
            super.viewDidLayoutSubviews()
            configure()
        }
        func configure() {
            var ancestor = parent
            while let controller = ancestor {
                // Only configure the presented sheet, not an unpresented child controller.
                if controller.presentingViewController != nil,
                   let sheet = controller.presentationController as? UISheetPresentationController {
                    if !sheet.prefersEdgeAttachedInCompactHeight { sheet.prefersEdgeAttachedInCompactHeight = true }
                    if sheet.widthFollowsPreferredContentSizeWhenEdgeAttached { sheet.widthFollowsPreferredContentSizeWhenEdgeAttached = false }
                    return
                }
                ancestor = controller.parent
            }
        }
    }
}

struct LibraryEntrance: ViewModifier {
    var delay: Double = 0
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var appeared = false
    func body(content: Content) -> some View {
        content.opacity(appeared ? 1 : 0)
            .offset(y: appeared || reduceMotion ? 0 : 8)
            .onAppear {
                withAnimation(reduceMotion ? nil : .smooth(duration: 0.3).delay(delay)) { appeared = true }
            }
    }
}

struct LibraryHeader: View {
    let count: Int
    let settings: () -> Void
    let defaultFolder: () -> Void
    let chooseDefaultFolder: () -> Void
    let appFolder: () -> Void
    let chooseFolder: () -> Void
    let installContent: () -> Void
    let installSystem: () -> Void
    @Environment(\.horizontalSizeClass) private var sizeClass
    var body: some View {
        LibraryGlassGroup {
            HStack(spacing: 16) {
                VStack(alignment: .leading, spacing: 3) {
                    Text("77U").font(.title2.bold()).accessibilityLabel("77U")
                    Text("\(count) \(count == 1 ? "game" : "games")")
                        .font(.caption).foregroundStyle(.secondary)
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .modifier(LibraryGlassSurface(cornerRadius: 20))
                Spacer(minLength: 8)
                Button(action: settings) {
                    Image(systemName: "gearshape")
                        .frame(width: 48, height: 48)
                        .modifier(LibraryGlassSurface(cornerRadius: 24, interactive: true))
                }
                    .controllerButtonStyle(.plain)
                    .accessibilityLabel("Settings")
                    .accessibilityIdentifier("library.settings")
                Menu {
                    Button("Add games…", systemImage: "folder.badge.plus", action: chooseFolder)
                    Button("Open default games folder", systemImage: "folder.badge.gearshape", action: defaultFolder)
                    Button("Choose default games folder…", systemImage: "folder.badge.plus", action: chooseDefaultFolder)
                    Button("Open 77U Games folder", systemImage: "folder", action: appFolder)
                    Divider()
                    Button("Install update or DLC…", systemImage: "square.and.arrow.down", action: installContent)
                    Button("Install Wii U system files…", systemImage: "house", action: installSystem)
                } label: {
                    Group {
                        if sizeClass == .compact { Image(systemName: "plus").frame(width: 48) }
                        else { Label("Add games", systemImage: "plus").padding(.horizontal, 14) }
                    }
                    .frame(height: 48)
                    .modifier(LibraryGlassSurface(cornerRadius: 24, interactive: true))
                }
                .controllerButtonStyle(.plain)
                .accessibilityLabel("Add and manage games")
                .accessibilityIdentifier("library.folder")
            }
        }
        .modifier(LibraryEntrance())
    }
}

struct LibraryHeaderBoundsKey: PreferenceKey {
    static var defaultValue: Anchor<CGRect>? { nil }
    static func reduce(value: inout Anchor<CGRect>?, nextValue: () -> Anchor<CGRect>?) {
        value = nextValue() ?? value
    }
}

/// A transient notice stays below the header without resizing the carousel.
struct LibraryNoticeOverlay<Notice: View>: ViewModifier {
    let horizontalInset: CGFloat
    @ViewBuilder let notice: () -> Notice

    func body(content: Content) -> some View {
        content.overlayPreferenceValue(LibraryHeaderBoundsKey.self) { bounds in
            GeometryReader { proxy in
                if let bounds {
                    notice()
                        .frame(maxWidth: .infinity, alignment: .center)
                        .padding(.horizontal, horizontalInset)
                        .offset(y: proxy[bounds].maxY + 4)
                        .allowsHitTesting(false)
                }
            }
        }
    }
}

struct LibraryGlassSurface: ViewModifier {
    var cornerRadius: CGFloat = 24
    var prominent = false
    var interactive = false
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @ObservedObject private var theme = MenuThemeStore.shared
    @ViewBuilder func body(content: Content) -> some View {
        if theme.configuration.enabled && reduceTransparency {
            content.background(Color(themeHex: theme.configuration.surface), in: RoundedRectangle(cornerRadius: cornerRadius))
        } else if reduceTransparency {
            content.background(prominent ? LibraryTheme.accent : Color(uiColor: .secondarySystemBackground),
                               in: RoundedRectangle(cornerRadius: cornerRadius))
        } else if #available(iOS 26, *) {
            if prominent {
                content.glassEffect(.regular.tint(LibraryTheme.accent).interactive(interactive), in: .rect(cornerRadius: cornerRadius))
            } else {
                content.glassEffect(.regular.interactive(interactive), in: .rect(cornerRadius: cornerRadius))
            }
        } else {
            content.background(.regularMaterial, in: RoundedRectangle(cornerRadius: cornerRadius))
        }
    }
}

struct LibraryGlassGroup<Content: View>: View {
    @ViewBuilder var content: () -> Content
    @ViewBuilder var body: some View {
        if #available(iOS 26, *) { GlassEffectContainer(spacing: 12) { content() } }
        else { content() }
    }
}

struct GameLibraryBackdrop: View {
    var titleURL: URL? = nil
    var isGameFile = false
    @ObservedObject private var theme = MenuThemeStore.shared
    var body: some View {
        if theme.configuration.enabled || theme.configuration.hasMedia { MenuThemeBackground(titleURL: titleURL, isGameFile: isGameFile) }
        else { Color(uiColor: .systemBackground).ignoresSafeArea() }
    }
}
