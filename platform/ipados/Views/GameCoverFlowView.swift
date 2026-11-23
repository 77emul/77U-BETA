import SwiftUI
import UIKit

struct GameCoverFlowView: View {
    let games: [GameLibraryEntry]
    let play: (GameLibraryEntry) async -> Bool
    let manageSaves: (GameLibraryEntry) -> Void
    let manageGraphics: (GameLibraryEntry) -> Void
    var isEnhancingArtwork = false

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @AppStorage("CemuIOS.SelectedLibraryGame") private var savedSelectedID = ""
    @State private var selectedID = ""
    @State private var selectionInitialized = false
    @State private var dragTranslation: CGFloat = 0
    @State private var dragStartIndex: Int?
    @State private var pressedID: String?
    @State private var launchingID: String?
    @Environment(\.colorScheme) private var colorScheme

    private var selectedIndex: Int { games.firstIndex(where: { $0.id == selectedID }) ?? 0 }

    var body: some View {
        GeometryReader { proxy in
            if UIDevice.current.userInterfaceIdiom == .phone {
                phoneLibrary(in: proxy)
            } else {
            let compact = proxy.size.width < 760
            let portrait = proxy.size.height > proxy.size.width
            let detailsHeight: CGFloat = compact ? 148 : 132
            let availableHeight = max(120, proxy.size.height - detailsHeight - 200)
            let cover = min(portrait ? 432 : 322,
                            max(100, min(proxy.size.width * (portrait ? 0.48 : compact ? 0.43 : 0.28), availableHeight / 1.25)))
            let step = cover * 1.24

            VStack(spacing: 12) {
                Spacer(minLength: 0)
                coverStage(size: cover, step: step)
                    .frame(height: cover * 1.25)

                selectionControls(step: step)
                if !games.isEmpty {
                    details(for: games[selectedIndex], compact: compact)
                        .frame(minHeight: detailsHeight)
                        .padding(.horizontal, compact ? 24 : 52)
                }
                Spacer(minLength: 0)
            }
            .padding(.bottom, max(24, proxy.safeAreaInsets.bottom + 10))
            .frame(width: proxy.size.width, height: proxy.size.height)
            }
        }
        .onAppear { restoreSelection() }
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.willResignActiveNotification)) { _ in
            dragStartIndex = nil
            dragTranslation = 0
            pressedID = nil
        }
        .onChange(of: games.map(\.id)) { restoreSelection() }
        .onChange(of: selectedID) { _, id in
            if savedSelectedID != id { savedSelectedID = id }
        }
        .allowsHitTesting(launchingID == nil)
        .task(id: launchingID) {
            guard let id = launchingID, let game = games.first(where: { $0.id == id }) else { return }
            do {
                if !reduceMotion { try await Task.sleep(for: .milliseconds(320)) }
                try Task.checkCancellation()
                // Keep the launch pose until the library hands off to the game.
                // Only a failed or declined launch should restore the cover.
                if !(await play(game)), !Task.isCancelled {
                    withAnimation(reduceMotion ? nil : .easeInOut(duration: 0.24)) { launchingID = nil }
                }
            } catch { launchingID = nil }
        }
        .accessibilityElement(children: .contain)
        .animation(reduceMotion ? nil : .easeInOut(duration: 0.35), value: colorScheme)
    }

    private func phoneLibrary(in proxy: GeometryProxy) -> some View {
        let landscape = proxy.size.width > proxy.size.height
        let side: CGFloat = 16
        let bottom: CGFloat = 12
        let cover = landscape
            ? max(80, min((proxy.size.height - bottom - 96) / 1.08, proxy.size.width * 0.30))
            : max(100, min(280, min(proxy.size.width * 0.68, (proxy.size.height - bottom - 260) / 1.25)))
        let step = cover * 1.18
        return VStack(spacing: 8) {
            if landscape {
                HStack(spacing: 24) {
                    phoneCovers(size: cover, step: step)
                        .frame(width: proxy.size.width * 0.36)
                    if !games.isEmpty {
                        details(for: games[selectedIndex], compact: true, inset: 14)
                    }
                }
            } else {
                Spacer(minLength: 4)
                phoneCovers(size: cover, step: step)
                Spacer(minLength: 6)
                if !games.isEmpty { details(for: games[selectedIndex], compact: true, inset: 14) }
                Spacer(minLength: 12)
            }
        }
        .padding(.horizontal, landscape ? max(side, max(proxy.safeAreaInsets.leading, proxy.safeAreaInsets.trailing)) : side)
        .padding(.bottom, bottom)
        .frame(width: proxy.size.width, height: proxy.size.height)
    }

    private func phoneCovers(size: CGFloat, step: CGFloat) -> some View {
        VStack(spacing: 0) {
            coverStage(size: size, step: step)
                .frame(height: size * 1.25)
            if games.count > 1 {
                selectionControls(step: step).frame(height: 44)
            }
        }
    }

    private func coverStage(size: CGFloat, step: CGFloat) -> some View {
        ZStack {
            positionedCovers(size: size, step: step)
                .overlay {
                    CoverFlowTouchSurface(
                        pressed: { point, width in
                            let id: String?
                            if let point {
                                let relative = (point.x - width / 2 - dragTranslation) / step
                                let index = selectedIndex + Int(relative.rounded())
                                id = games.indices.contains(index) && abs(relative - relative.rounded()) * step <= size / 2
                                    ? games[index].id : nil
                            } else { id = nil }
                            withAnimation(reduceMotion ? nil : .easeOut(duration: 0.10)) { pressedID = id }
                        },
                        began: {
                            dragStartIndex = selectedIndex
                            pressedID = nil
                        },
                        moved: { translation in
                            let start = dragStartIndex ?? selectedIndex
                            var transaction = Transaction()
                            transaction.disablesAnimations = true
                            withTransaction(transaction) {
                                dragTranslation = GameCoverFlowGeometry.rubberBandedTranslation(
                                    translation, currentIndex: start, gameCount: games.count, step: step)
                            }
                        },
                        ended: { translation, velocity, cancelled in
                            let start = dragStartIndex ?? selectedIndex
                            let target = cancelled ? start : GameCoverFlowGeometry.touchTarget(
                                currentIndex: start, gameCount: games.count,
                                translation: translation, velocity: velocity, step: step)
                            dragStartIndex = nil
                            if target == selectedIndex {
                                withAnimation(reduceMotion ? nil : .spring(response: 0.43, dampingFraction: 0.78)) {
                                    dragTranslation = 0
                                }
                            } else { select(target, step: step) }
                        },
                        tapped: { point, width in
                            guard point.y >= 0, point.y <= size + 12 else { return }
                            let relative = (point.x - width / 2 - dragTranslation) / step
                            let offset = Int(relative.rounded())
                            guard abs(relative - CGFloat(offset)) * step <= size / 2 else { return }
                            if offset == 0, !games.isEmpty {
                                beginLaunch(games[selectedIndex])
                            } else { select(selectedIndex + offset, step: step) }
                        })
                        .accessibilityHidden(true)
                }
        }
        .accessibilityIdentifier("library.carousel")
    }

    private func positionedCovers(size: CGFloat, step: CGFloat) -> some View {
        let visible = GameCoverFlowGeometry.visibleIndices(currentIndex: selectedIndex,
            gameCount: games.count, translation: dragTranslation, step: step)
        return GeometryReader { stage in
            ZStack {
            ForEach(visible.map { games[$0] }) { game in
                let index = games.firstIndex(where: { $0.id == game.id }) ?? selectedIndex
                CoverCardView(game: games[index], index: index, selectedIndex: selectedIndex,
                    continuousDrag: dragTranslation / step, step: step, size: size,
                    isSelected: index == selectedIndex && abs(dragTranslation) < step * 0.45,
                    isPressed: pressedID == games[index].id,
                    isLaunching: launchingID == games[index].id,
                    reduceMotion: reduceMotion,
                    isEnhancingArtwork: isEnhancingArtwork,
                    onSelect: {
                        if index == selectedIndex { beginLaunch(games[index]) }
                        else { select(index, step: step) }
                    })
                    .opacity(launchingID != nil && launchingID != game.id ? 0.55 : 1)
                    .allowsHitTesting(false)
                    .position(x: stage.size.width / 2, y: stage.size.height / 2)
                    .zIndex(50 - Double(abs(CGFloat(index - selectedIndex) + dragTranslation / step)) * 10)
            }
            }
            .frame(width: stage.size.width, height: stage.size.height)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    @ViewBuilder
    private func selectionControls(step: CGFloat) -> some View {
        if games.count > 1 {
            HStack(spacing: 12) {
                Button { select(selectedIndex - 1, step: step) } label: {
                    Image(systemName: "chevron.left").frame(width: 44, height: 44)
                }
                .disabled(selectedIndex == 0)
                .opacity(selectedIndex == 0 ? 0.35 : 1)
                .accessibilityLabel("Previous game")
                .accessibilityIdentifier("library.previous")
                HStack(spacing: 6) {
                    if games.count <= 12 {
                        ForEach(games.indices, id: \.self) { index in
                            Capsule()
                                .fill(LibraryTheme.text(for: colorScheme).opacity(index == selectedIndex ? 1 : 0.20))
                                .frame(width: index == selectedIndex ? 18 : 5, height: 5)
                        }
                    } else {
                        Text("\(selectedIndex + 1) / \(games.count)")
                            .font(.caption.monospacedDigit())
                            .frame(minWidth: 60, minHeight: 44)
                    }
                }
                .accessibilityElement(children: .ignore)
                .accessibilityLabel("Game \(selectedIndex + 1) of \(games.count)")
                .animation(reduceMotion ? nil : .spring(response: 0.35, dampingFraction: 0.8), value: selectedIndex)
                Button { select(selectedIndex + 1, step: step) } label: {
                    Image(systemName: "chevron.right").frame(width: 44, height: 44)
                }
                .disabled(selectedIndex >= games.count - 1)
                .opacity(selectedIndex >= games.count - 1 ? 0.35 : 1)
                .accessibilityLabel("Next game")
                .accessibilityIdentifier("library.next")
            }
            .font(.system(size: 12, weight: .bold))
            .controllerButtonStyle(.plain)
            .foregroundStyle(LibraryTheme.text(for: colorScheme))
        }
    }

    private func details(for game: GameLibraryEntry, compact: Bool, inset: CGFloat = 18) -> some View {
        LibraryGameDock(game: game, compact: compact, inset: inset,
                        play: beginLaunch, manageSaves: manageSaves, manageGraphics: manageGraphics)
    }

    private func beginLaunch(_ game: GameLibraryEntry) {
        guard launchingID == nil else { return }
        withAnimation(reduceMotion ? nil : .easeInOut(duration: 0.32)) {
            pressedID = nil
            launchingID = game.id
        }
    }

    private func restoreSelection() {
        let restored = GameCoverFlowGeometry.initialSelectionID(
            savedID: selectionInitialized ? selectedID : savedSelectedID, gameIDs: games.map(\.id))
        selectionInitialized = true
        if selectedID != restored { selectedID = restored }
        dragStartIndex = nil
        dragTranslation = 0
    }

    private func select(_ index: Int, step: CGFloat) {
        guard !games.isEmpty else { return }
        let target = min(games.count - 1, max(0, index))
        guard target != selectedIndex else { return }
        // Stable game identities let SwiftUI animate directly from the current dragged
        // positions to the new selection without an intermediate unanimated rebase.
        withAnimation(reduceMotion ? nil : .spring(response: 0.43, dampingFraction: 0.78)) {
            selectedID = games[target].id
            dragTranslation = 0
        }
        UISelectionFeedbackGenerator().selectionChanged()
    }

    private func settle(_ value: DragGesture.Value, step: CGFloat) {
        let target = GameCoverFlowGeometry.settledIndex(currentIndex: selectedIndex, gameCount: games.count,
            translation: value.translation.width, predictedTranslation: value.predictedEndTranslation.width, step: step)
        if target == selectedIndex {
            withAnimation(reduceMotion ? nil : .spring(response: 0.43, dampingFraction: 0.78)) { dragTranslation = 0 }
        } else { select(target, step: step) }
    }
}

private struct CoverCardView: View {
    let game: GameLibraryEntry
    let index: Int
    let selectedIndex: Int
    let continuousDrag: CGFloat
    let step: CGFloat
    let size: CGFloat
    let isSelected: Bool
    let isPressed: Bool
    let isLaunching: Bool
    let reduceMotion: Bool
    let isEnhancingArtwork: Bool
    let onSelect: () -> Void
    @Environment(\.colorScheme) private var colorScheme

    var body: some View {
        let relative: CGFloat = CGFloat(index - selectedIndex) + continuousDrag
        let distance: CGFloat = abs(relative)
        let scale: CGFloat = max(0.72, 1.0 - min(distance, 2.5) * 0.13)
        let clampedRel: CGFloat = relative.clamped(to: -1.6...1.6)
        let angle: Double = reduceMotion ? 0 : Double(-clampedRel) * 11.0
        let xOffset: CGFloat = relative * step
        let opacity: Double = max(0.22, 1.0 - Double(max(0, distance - 1)) * 0.28)
        let z: Double = Double(50.0 - distance * 10.0)

        Button(action: onSelect) {
        VStack(spacing: 12) {
            coverBody(isSelected: isSelected)
            coverReflection
        }
        .frame(width: size, height: size * 1.25, alignment: .top)
        .contentShape(Rectangle())
        }
        .controllerButtonStyle(CoverPressStyle(reduceMotion: reduceMotion))
        .scaleEffect(isEnhancingArtwork && !reduceMotion ? 0.92 : 1,
                     anchor: UIDevice.current.userInterfaceIdiom == .phone ? .top : .center)
        .animation(reduceMotion ? nil : .easeInOut(duration: 0.35), value: isEnhancingArtwork)
        .scaleEffect(scale)
        .scaleEffect(reduceMotion ? 1 : (isLaunching ? 1.035 : (isPressed ? 0.98 : 1)))
        .opacity(isLaunching && !reduceMotion ? 0.82 : 1)
        .rotation3DEffect(.degrees(angle), axis: (x: 0.0, y: 1.0, z: 0.0), anchor: .center, perspective: 0.18)
        .offset(x: xOffset)
        .opacity(opacity)
        .zIndex(z)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(game.title)
        .accessibilityAddTraits(.isButton)
        .accessibilityAddTraits(isSelected ? .isSelected : [])
        .accessibilityAction { onSelect() }
        .animation(reduceMotion ? nil : .easeInOut(duration: 0.35), value: colorScheme)
    }

    private var coverReflection: some View {
        // Flip the entire cover first; then crop its bottom edge into a short reflection.
        coverArtwork
            .scaleEffect(y: -1)
            .frame(height: size * 0.20, alignment: .top)
            .clipped()
            .mask(LinearGradient(stops: [
                .init(color: .white.opacity(isSelected ? 0.16 : 0.07), location: 0),
                .init(color: .white.opacity(isSelected ? 0.04 : 0.02), location: 0.45),
                .init(color: .clear, location: 1)
            ], startPoint: .top, endPoint: .bottom))
            .allowsHitTesting(false)
            .accessibilityHidden(true)
    }

    private var coverArtwork: some View {
        let isLight = colorScheme == .light
        return Group {
            if let image = game.icon {
                Image(uiImage: image)
                    .resizable()
                    .interpolation(.high)
                    .scaledToFit()
            } else {
                ZStack {
                    LinearGradient(
                        colors: isLight
                            ? [Color(white: 0.94), Color(white: 0.86)]
                            : [Color(white: 0.18), Color(white: 0.09)],
                        startPoint: .top,
                        endPoint: .bottom
                    )
                    Image(systemName: "gamecontroller.fill")
                        .font(.system(size: size * 0.22))
                        .foregroundStyle(isLight ? Color.black.opacity(0.25) : Color.white.opacity(0.28))
                }
            }
        }
        .frame(width: size, height: size)
        .background(isLight ? Color(white: 0.95) : Color(white: 0.08))
        .clipShape(RoundedRectangle(cornerRadius: 30, style: .continuous))
    }

    private func coverBody(isSelected: Bool) -> some View {
        coverArtwork
            .overlay {
                RoundedRectangle(cornerRadius: 30, style: .continuous)
                    .stroke(LinearGradient(
                        colors: colorScheme == .light
                            ? [Color.black.opacity(isSelected ? 0.32 : 0.12),
                               Color.black.opacity(0.04),
                               Color.black.opacity(0.18)]
                            : [.white.opacity(isSelected ? 0.65 : 0.28),
                               .white.opacity(0.06),
                               .black.opacity(0.5)],
                        startPoint: .topLeading,
                        endPoint: .bottomTrailing
                    ),
                    lineWidth: 0.8)
            }
            .padding(6)
            .background(colorScheme == .light ? Color.white : Color(white: 0.10),
                        in: RoundedRectangle(cornerRadius: 36, style: .continuous))
            .overlay {
                RoundedRectangle(cornerRadius: 36, style: .continuous)
                    .strokeBorder(Color.primary.opacity(0.08), lineWidth: 0.7)
            }
            .shadow(
                color: .black.opacity(colorScheme == .light ? 0.10 : 0.32),
                radius: 14,
                y: 8
            )
            .accessibilityLabel(game.title)
            .accessibilityAddTraits(isSelected ? .isSelected : [])
    }

}


private extension Comparable {
    func clamped(to range: ClosedRange<Self>) -> Self {
        min(range.upperBound, max(range.lowerBound, self))
    }
}

private struct CoverPressStyle: ButtonStyle {
    let reduceMotion: Bool
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed && !reduceMotion ? 0.97 : 1)
            .animation(reduceMotion ? nil : .spring(response: 0.26, dampingFraction: 0.8),
                       value: configuration.isPressed)
    }
}

// UIKit arbitrates the single finger contact before any selection action can fire.
private struct CoverFlowTouchSurface: UIViewRepresentable {
    let pressed: (CGPoint?, CGFloat) -> Void
    let began: () -> Void
    let moved: (CGFloat) -> Void
    let ended: (CGFloat, CGFloat, Bool) -> Void
    let tapped: (CGPoint, CGFloat) -> Void

    func makeCoordinator() -> Coordinator { Coordinator(self) }
    func makeUIView(context: Context) -> UIView {
        let view = TouchView()
        view.pressed = pressed
        view.backgroundColor = .clear
        view.isMultipleTouchEnabled = false
        let pan = UIPanGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.pan(_:)))
        pan.maximumNumberOfTouches = 1
        pan.delegate = context.coordinator
        let tap = UITapGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.tap(_:)))
        tap.require(toFail: pan)
        view.addGestureRecognizer(pan)
        view.addGestureRecognizer(tap)
        return view
    }
    func updateUIView(_ uiView: UIView, context: Context) {
        context.coordinator.owner = self
        (uiView as? TouchView)?.pressed = pressed
    }

    final class TouchView: UIView {
        var pressed: ((CGPoint?, CGFloat) -> Void)?

        override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
            super.touchesBegan(touches, with: event)
            pressed?(touches.first?.location(in: self), bounds.width)
        }

        override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
            super.touchesEnded(touches, with: event)
            pressed?(nil, bounds.width)
        }

        override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
            super.touchesCancelled(touches, with: event)
            pressed?(nil, bounds.width)
        }
    }

    final class Coordinator: NSObject, UIGestureRecognizerDelegate {
        var owner: CoverFlowTouchSurface
        init(_ owner: CoverFlowTouchSurface) { self.owner = owner }
        func gestureRecognizerShouldBegin(_ gestureRecognizer: UIGestureRecognizer) -> Bool {
            guard let pan = gestureRecognizer as? UIPanGestureRecognizer else { return true }
            let velocity = pan.velocity(in: pan.view)
            return abs(velocity.x) > abs(velocity.y)
        }
        @objc func pan(_ recognizer: UIPanGestureRecognizer) {
            let translation = recognizer.translation(in: recognizer.view).x
            switch recognizer.state {
            case .began: owner.began(); owner.moved(translation)
            case .changed: owner.moved(translation)
            case .ended: owner.ended(translation, recognizer.velocity(in: recognizer.view).x, false)
            case .cancelled, .failed: owner.ended(translation, 0, true)
            default: break
            }
        }
        @objc func tap(_ recognizer: UITapGestureRecognizer) {
            guard recognizer.state == .ended, let view = recognizer.view else { return }
            owner.tapped(recognizer.location(in: view), view.bounds.width)
        }
    }
}
