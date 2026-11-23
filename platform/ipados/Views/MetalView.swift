import SwiftUI
import UIKit

/// The underlying renderer layers keep their identity when screens are swapped.
struct MetalView: UIViewRepresentable {
    var rendersDRC = false
    var nativeResolution = false
    var onLayerAttached: ((Bool) -> Void)?

    func makeUIView(context: Context) -> EmulatorMetalView {
        let view = EmulatorMetalView()
        configure(view)
        return view
    }
    func updateUIView(_ view: EmulatorMetalView, context: Context) { configure(view) }
    private func configure(_ view: EmulatorMetalView) {
        view.rendersDRC = rendersDRC
        view.nativeResolution = nativeResolution
        view.onLayerAttachedChange = { attached in
            onLayerAttached?(attached)
        }
    }
}

struct DRCPreviewView: UIViewRepresentable {
    var isActive: Bool
    var nativeResolution: Bool
    func makeUIView(context: Context) -> DRCPreviewMetalView { DRCPreviewMetalView() }
    func updateUIView(_ view: DRCPreviewMetalView, context: Context) {
        view.nativeResolution = nativeResolution
        view.previewActive = isActive
        // New frames arrive through the renderer publication callback. A SwiftUI
        // diagnostic refresh must not submit a second copy of the same frame.
    }
}

/// Shared by the live display and layout checks; all dimensions are in points.
enum PlayScreenGeometry {
    static func previewSize(in size: CGSize, scale: Double) -> CGSize {
        let width = min(size.width * 0.44, min(size.height * 0.42 * 854 / 480, 250 * scale))
        return CGSize(width: width, height: width * 480 / 854)
    }
    static func movementArea(in size: CGSize, preview: CGSize) -> CGRect {
        CGRect(x: 16 + preview.width / 2, y: 90 + preview.height / 2,
               width: max(0, size.width - 32 - preview.width),
               height: max(0, size.height - 110 - preview.height))
    }
    static func center(in size: CGSize, preview: CGSize, configuration: PlayConfiguration) -> CGPoint {
        let area = movementArea(in: size, preview: preview)
        let anchor: PadPreviewAnchor
        if let custom = configuration.previewAnchor { anchor = custom }
        else {
            switch configuration.previewPosition {
            case .top: anchor = .init(x: 0.5, y: 0)
            case .left: anchor = .init(x: 0, y: 0)
            case .right: anchor = .init(x: 1, y: 0)
            case .bottom: anchor = .init(x: 0.5, y: 1)
            }
        }
        return CGPoint(x: area.minX + min(1, max(0, anchor.x)) * area.width,
                       y: area.minY + min(1, max(0, anchor.y)) * area.height)
    }
    static func anchor(for point: CGPoint, in size: CGSize, preview: CGSize) -> PadPreviewAnchor {
        let area = movementArea(in: size, preview: preview)
        return .init(x: min(1, max(0, (point.x - area.minX) / max(1, area.width))),
                     y: min(1, max(0, (point.y - area.minY) / max(1, area.height))))
    }
}

struct MetalDisplayStack: View {
    var onLayerAttached: ((Bool) -> Void)?
    var showsDRCPreview: Bool
    @ObservedObject var preferences: PlayPreferences
    @ObservedObject private var externalDisplay = ExternalDisplayCoordinator.shared
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @GestureState private var previewTranslation = CGSize.zero
    private var configuration: PlayConfiguration { preferences.configuration }

    var body: some View {
        GeometryReader { geometry in
            let bounds = geometry.size
            let portrait = bounds.height > bounds.width
            let phone = UIDevice.current.userInterfaceIdiom == .phone
            let phonePortrait = phone && portrait
            // Phone content is already inside the system safe area. The iPad
            // keeps its existing edge-to-edge surfaces and renderer dimensions.
            let phoneDeck: CGFloat = phonePortrait ? 372 : 0
            let phoneArea = phone ? CGRect(x: 0, y: phonePortrait ? 56 : 0,
                                           width: bounds.width,
                                           height: max(1, bounds.height - (phonePortrait ? 56 + phoneDeck : 0))) : .zero
            let docked = externalDisplay.isConnected
            let split = !docked && configuration.display == .split
            let padMain = docked || configuration.display == .gamepad
            let controllerDeckHeight = docked ? (phonePortrait ? phoneDeck : min(max(bounds.height * 0.29, 210), 270)) : 0
            let gamePadAreaHeight = max(1, bounds.height - controllerDeckHeight)
            let preview = PlayScreenGeometry.previewSize(in: bounds, scale: configuration.previewScale)
            let savedCenter = PlayScreenGeometry.center(in: bounds, preview: preview, configuration: configuration)
            let draggedAnchor = PlayScreenGeometry.anchor(for: CGPoint(x: savedCenter.x + previewTranslation.width,
                                                                        y: savedCenter.y + previewTranslation.height),
                                                          in: bounds, preview: preview)
            let area = PlayScreenGeometry.movementArea(in: bounds, preview: preview)
            let smallCenter = CGPoint(x: area.minX + draggedAnchor.x * area.width,
                                      y: area.minY + draggedAnchor.y * area.height)
            let regularTVSize = padMain ? preview : CGSize(width: split && !portrait ? bounds.width * 0.66 : bounds.width,
                                                     height: split && portrait ? bounds.height * 0.59 : bounds.height)
            let regularTVCenter = padMain ? smallCenter : CGPoint(x: regularTVSize.width / 2, y: regularTVSize.height / 2)
            let regularPadWidth = padMain ? min(bounds.width, (docked ? gamePadAreaHeight : bounds.height) * 854 / 480) :
                (split ? (portrait ? min(bounds.width * 0.7, bounds.height * 0.36 * 854 / 480) : bounds.width * 0.32) : preview.width)
            let regularPadCenter = padMain ? CGPoint(x: bounds.width / 2, y: (docked ? gamePadAreaHeight : bounds.height) / 2) :
                (split ? (portrait ? CGPoint(x: bounds.width / 2, y: bounds.height * 0.78) :
                           CGPoint(x: bounds.width * 0.83, y: bounds.height / 2)) : smallCenter)
            let phonePortraitAreaHeight = phoneArea.height * (split && portrait ? 0.59 : 1)
            let phoneTVWidth = phonePortrait
                ? min(phoneArea.width, phonePortraitAreaHeight * 16.0 / 9.0)
                : phoneArea.width * (split && !portrait ? 0.66 : 1)
            let phoneTVHeight = phonePortrait
                ? (phoneTVWidth * 9.0 / 16.0)
                : phoneArea.height * (split && portrait ? 0.59 : 1)
            let tvSize = phone && !padMain
                ? CGSize(width: phoneTVWidth, height: phoneTVHeight)
                : regularTVSize
            let tvCenter = phone && !padMain
                ? CGPoint(x: phoneArea.midX, y: phoneArea.minY + (phonePortrait ? phonePortraitAreaHeight / 2 : tvSize.height / 2))
                : regularTVCenter
            let padWidth = phone && padMain ? min(phoneArea.width, phoneArea.height * 854 / 480) :
                (phone && split ? (portrait ? min(phoneArea.width * 0.7, phoneArea.height * 0.36 * 854 / 480) : phoneArea.width * 0.32) : regularPadWidth)
            let padCenter = phone && padMain ? CGPoint(x: phoneArea.midX, y: phoneArea.midY) :
                (phone && split ? (portrait ? CGPoint(x: phoneArea.midX, y: phoneArea.minY + phoneArea.height * 0.79) :
                                   CGPoint(x: phoneArea.minX + phoneArea.width * 0.83, y: phoneArea.midY)) : regularPadCenter)
            let previewVisible = docked || showsDRCPreview || preferences.editingPreview

            ZStack(alignment: .topLeading) {
                RoutedTVView(nativeResolution: configuration.nativeDisplayResolution,
                             onLayerAttached: onLayerAttached)
                    .frame(width: tvSize.width, height: tvSize.height)
                    .clipShape(RoundedRectangle(cornerRadius: padMain ? 10 : 0))
                    .overlay(RoundedRectangle(cornerRadius: padMain ? 10 : 0)
                        .strokeBorder(.white.opacity(padMain ? 0.22 : 0), lineWidth: 1))
                    .position(tvCenter)
                    .opacity(docked ? 0 : (!padMain || previewVisible ? 1 : 0))
                    .zIndex(padMain ? 2 : 0)
                    .allowsHitTesting(false)
                MetalView(rendersDRC: true, onLayerAttached: onLayerAttached)
                    .frame(width: 1, height: 1).opacity(0).allowsHitTesting(false)
                DRCPreviewView(isActive: previewVisible || split || padMain,
                               nativeResolution: configuration.nativeDisplayResolution)
                    .frame(width: padWidth, height: padWidth * 480 / 854)
                    .background(.black)
                    .clipShape(RoundedRectangle(cornerRadius: padMain ? 0 : 10))
                    .overlay(RoundedRectangle(cornerRadius: padMain ? 0 : 10)
                        .strokeBorder(.white.opacity(padMain ? 0 : 0.22), lineWidth: 1))
                    .position(padCenter)
                    .opacity(previewVisible || split || padMain ? 1 : 0)
                    .allowsHitTesting(!preferences.editingPreview && (previewVisible || split || padMain))
                    .accessibilityLabel("Wii U GamePad touch screen")
                    .zIndex(1)
                if docked {
                    VStack(spacing: 0) {
                        Spacer(minLength: 0)
                        Rectangle()
                            .fill(LinearGradient(colors: [.black.opacity(0.82), .black],
                                                 startPoint: .top, endPoint: .bottom))
                            .frame(height: controllerDeckHeight)
                            .overlay(alignment: .top) {
                                Rectangle().fill(.white.opacity(0.12)).frame(height: 1)
                            }
                    }
                    .allowsHitTesting(false)
                    .zIndex(2)
                }
                if preferences.editingPreview && !split && !docked {
                    RoundedRectangle(cornerRadius: 10)
                        .fill(.black.opacity(0.15))
                        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(.white.opacity(0.8), lineWidth: 2))
                        .overlay {
                            Label(padMain ? "Move TV" : "Move GamePad", systemImage: "arrow.up.and.down.and.arrow.left.and.right")
                                .font(.system(size: 13, weight: .medium))
                                .padding(10).background(.black.opacity(0.75), in: Capsule())
                        }
                        .frame(width: preview.width, height: preview.height)
                        .contentShape(Rectangle())
                        .position(smallCenter)
                        .gesture(DragGesture(minimumDistance: 0)
                            .updating($previewTranslation) { value, translation, _ in translation = value.translation }
                            .onEnded { value in
                                preferences.configuration.previewAnchor = PlayScreenGeometry.anchor(
                                    for: CGPoint(x: savedCenter.x + value.translation.width, y: savedCenter.y + value.translation.height),
                                    in: bounds, preview: preview)
                            })
                        .accessibilityLabel("Move small screen")
                        .zIndex(3)
                }
            }
            .animation(reduceMotion ? nil : .easeOut(duration: 0.18), value: showsDRCPreview)
        }
        .background(Color.black.ignoresSafeArea())
    }
}
