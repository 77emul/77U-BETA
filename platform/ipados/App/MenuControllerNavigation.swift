import SwiftUI
import GameController
import UIKit

struct MenuControllerCapture {
    private var menuWasVisible = false
    private var waitingForRelease = false
    mutating func update(gameActive: Bool, menuVisible: Bool, controlsNeutral: Bool) -> Bool {
        guard gameActive else { menuWasVisible = false; waitingForRelease = false; return false }
        if menuVisible { menuWasVisible = true; waitingForRelease = false; return true }
        if menuWasVisible { waitingForRelease = true; menuWasVisible = false }
        if controlsNeutral { waitingForRelease = false }
        return waitingForRelease
    }
}

/// Poll snapshots rather than replacing handlers owned by the emulator input backend.
struct MenuControllerNavigation: UIViewRepresentable {
    func makeUIView(context: Context) -> ControllerMenuView { ControllerMenuView() }
    func updateUIView(_ view: ControllerMenuView, context: Context) {}
    static func dismantleUIView(_ view: ControllerMenuView, coordinator: ()) { view.stop() }
}

final class ControllerMenuView: UIView {
    private var timer: Timer?
    private var selected: NSObject?
    private weak var selectedRoot: UIViewController?
    private var lastInput = ""
    private var menuPressed = false
    private var capture = MenuControllerCapture()
    private var lastMove = Date.distantPast
    private let ring = UIView()
    override init(frame: CGRect) {
        super.init(frame: frame)
        isUserInteractionEnabled = false
        ring.isUserInteractionEnabled = false
        ring.layer.borderWidth = 3; ring.layer.cornerRadius = 10
        ring.backgroundColor = .clear; ring.isHidden = true
        timer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.poll() }
        }
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
    func stop() { timer?.invalidate(); timer = nil; selected = nil; selectedRoot = nil; ring.removeFromSuperview(); CemuBridge_SetMenuControllerCapture(false) }
    private func candidates(_ object: NSObject, depth: Int = 0) -> [NSObject] {
        guard depth < 30 else { return [] }
        if let view = object as? UIView, view.isHidden || view.alpha < 0.01 { return [] }
        if object.isAccessibilityElement {
            let traits = object.accessibilityTraits
            if traits.contains(.button) || traits.contains(.adjustable) || traits.contains(.link) {
                let frame = object.accessibilityFrame
                if !traits.contains(.notEnabled), !frame.isEmpty, let window, window.convert(window.bounds, to: nil).intersects(frame) { return [object] }
            }
        }
        if let elements = object.accessibilityElements as? [NSObject], !elements.isEmpty {
            return elements.filter { $0 !== object }.flatMap { candidates($0, depth: depth + 1) }
        }
        let count = object.accessibilityElementCount()
        if count > 0 && count < 2_000 {
            return (0..<count).compactMap { object.accessibilityElement(at: $0) as? NSObject }
                .filter { $0 !== object }.flatMap { candidates($0, depth: depth + 1) }
        }
        return (object as? UIView)?.subviews.flatMap { candidates($0, depth: depth + 1) } ?? []
    }
    private func registeredTargets(in view: UIView) -> [MenuControlAnchorView] {
        guard !view.isHidden, view.alpha > 0.01 else { return [] }
        if let target = view as? MenuControlAnchorView {
            guard target.enabled, !target.accessibilityFrame.isEmpty,
                  let window, window.convert(window.bounds, to: nil).intersects(target.accessibilityFrame) else { return [] }
            return [target]
        }
        return view.subviews.flatMap { registeredTargets(in: $0) }
    }
    private func navigationController(in controller: UIViewController?) -> UINavigationController? {
        guard let controller else { return nil }
        if let nav = controller as? UINavigationController, nav.viewControllers.count > 1 { return nav }
        return controller.children.reversed().compactMap { navigationController(in: $0) }.first
    }
    private func scroll(in view: UIView, forward: Bool) -> Bool {
        if let scroll = view as? UIScrollView, scroll.contentSize.height > scroll.bounds.height {
            let minY = -scroll.adjustedContentInset.top
            let maxY = max(minY, scroll.contentSize.height - scroll.bounds.height + scroll.adjustedContentInset.bottom)
            let target = min(maxY, max(minY, scroll.contentOffset.y + (forward ? 1 : -1) * scroll.bounds.height * 0.65))
            guard abs(target - scroll.contentOffset.y) > 1 else { return false }
            scroll.setContentOffset(CGPoint(x: scroll.contentOffset.x, y: target), animated: !UIAccessibility.isReduceMotionEnabled)
            return true
        }
        return view.subviews.contains { scroll(in: $0, forward: forward) }
    }
    private func updateRing(in window: UIWindow) {
        guard let selected else { ring.isHidden = true; return }
        let frame = selected.accessibilityFrame
        if frame.isEmpty || !window.convert(window.bounds, to: nil).intersects(frame) || (selected as? MenuControlAnchorView)?.enabled == false {
            self.selected = nil; ring.isHidden = true; return
        }
        if ring.superview !== window { window.addSubview(ring) }
        ring.layer.borderColor = (MenuThemeStore.shared.configuration.enabled ? UIColor(Color(themeHex: MenuThemeStore.shared.configuration.accent)) : UIColor.tintColor).cgColor
        ring.frame = window.convert(frame, from: nil).insetBy(dx: -3, dy: -3)
        ring.isHidden = false
    }
    func poll(gamepad: GCExtendedGamepad? = nil, gameActive: Bool = GameOrientationCoordinator.isGameActive) {
        guard let window, UIApplication.shared.applicationState == .active else { CemuBridge_SetMenuControllerCapture(false); return }
        guard gameActive else {
            selected = nil
            ring.isHidden = true
            menuPressed = false
            lastInput = ""
            CemuBridge_SetMenuControllerCapture(capture.update(gameActive: false, menuVisible: false, controlsNeutral: true))
            return
        }
        var root = window.rootViewController
        while let presented = root?.presentedViewController { root = presented }
        if selectedRoot !== root { selected = nil; ring.isHidden = true; selectedRoot = root }
        let currentPad = gamepad ?? GCController.current?.extendedGamepad ?? GCController.controllers().compactMap({ $0.extendedGamepad }).first
        let neutral: Bool
        if let pad = currentPad {
            let axes = [pad.dpad.xAxis.value, pad.dpad.yAxis.value, pad.leftThumbstick.xAxis.value, pad.leftThumbstick.yAxis.value, pad.rightThumbstick.xAxis.value, pad.rightThumbstick.yAxis.value]
            neutral = pad.allButtons.allSatisfy { !$0.isPressed } && axes.allSatisfy { $0.isFinite && abs($0) < 0.2 }
        } else { neutral = true }
        CemuBridge_SetMenuControllerCapture(capture.update(gameActive: gameActive, menuVisible: root?.presentingViewController != nil, controlsNeutral: neutral))
        guard let pad = currentPad else { ring.isHidden = true; return }
        let pressed = pad.buttonMenu.isPressed
        if pressed && !menuPressed && root?.presentingViewController == nil {
            NotificationCenter.default.post(name: Notification.Name("77U.ControllerMenu"), object: nil)
        }
        menuPressed = pressed
        guard root?.presentingViewController != nil else { ring.isHidden = true; return }
        let x = abs(pad.dpad.xAxis.value) > 0.5 ? pad.dpad.xAxis.value : pad.leftThumbstick.xAxis.value
        let y = abs(pad.dpad.yAxis.value) > 0.5 ? pad.dpad.yAxis.value : pad.leftThumbstick.yAxis.value
        let input = pad.buttonA.isPressed ? "activate" : pad.buttonB.isPressed ? "back" : y > 0.6 ? "up" : y < -0.6 ? "down" : x > 0.6 ? "right" : x < -0.6 ? "left" : ""
        defer { lastInput = input }
        guard !input.isEmpty else { updateRing(in: window); return }
        let repeated = input == lastInput
        guard !repeated || (["up", "down", "left", "right"].contains(input) && Date().timeIntervalSince(lastMove) > 0.25) else { return }
        lastMove = Date()
        if input == "back" {
            if root?.view.accessibilityPerformEscape() == true {
                ring.isHidden = true; selected = nil; return
            } else if let nav = navigationController(in: root) { nav.popViewController(animated: !UIAccessibility.isReduceMotionEnabled) }
            else if root?.presentingViewController != nil, root?.isModalInPresentation != true { root?.dismiss(animated: !UIAccessibility.isReduceMotionEnabled) }
            ring.isHidden = true; selected = nil; return
        }
        guard let rootView = root?.view else { return }
        let registered = registeredTargets(in: rootView)
        let native = candidates(rootView).filter { candidate in
            !registered.contains { $0.accessibilityFrame.intersects(candidate.accessibilityFrame) }
        }
        let items: [NSObject] = (registered.map { $0 as NSObject } + native).sorted {
            let a = $0.accessibilityFrame, b = $1.accessibilityFrame
            return a.midY == b.midY ? a.minX < b.minX : a.midY < b.midY
        }
        guard !items.isEmpty else { ring.isHidden = true; return }
        let current = items.firstIndex { item in
            guard let selected else { return false }
            return item === selected || (item.accessibilityLabel != nil && item.accessibilityLabel == selected.accessibilityLabel && item.accessibilityFrame == selected.accessibilityFrame)
        }
        if let current { selected = items[current] }
        if input == "activate", let current {
            let item = items[current]
            if !item.accessibilityActivate(), let control = item as? UIControl { control.sendActions(for: .primaryActionTriggered) }
        }
        else if let current, items[current].accessibilityTraits.contains(.adjustable), input == "right" || input == "left" {
            if input == "right" { items[current].accessibilityIncrement() } else { items[current].accessibilityDecrement() }
        } else {
            let delta = input == "up" || input == "left" ? -1 : 1
            if let current, (current == items.count - 1 && delta > 0) || (current == 0 && delta < 0), scroll(in: rootView, forward: delta > 0) {
                ring.isHidden = true; selected = nil; return
            }
            let next = current.map { ($0 + delta + items.count) % items.count } ?? 0
            selected = items[next]
        }
        updateRing(in: window)
    }
}

/// Explicit targets keep controller actions available even when SwiftUI's accessibility tree is dormant.
private final class MenuControlAnchorView: UIView {
    var activate: (() -> Void)?
    var adjust: ((Int) -> Void)?
    var enabled = true
    override var accessibilityFrame: CGRect {
        get { guard let window else { return .zero }; return window.convert(convert(bounds, to: window), to: nil) }
        set { }
    }
    override func accessibilityActivate() -> Bool { guard enabled, let activate else { return false }; activate(); return true }
    override func accessibilityIncrement() { if enabled { adjust?(1) } }
    override func accessibilityDecrement() { if enabled { adjust?(-1) } }
}

private struct MenuControlAnchor: UIViewRepresentable {
    @Environment(\.isEnabled) private var enabled
    var activate: (() -> Void)? = nil
    var adjust: ((Int) -> Void)? = nil
    func makeUIView(context: Context) -> MenuControlAnchorView {
        let view = MenuControlAnchorView()
        view.isUserInteractionEnabled = false
        view.isAccessibilityElement = false
        return view
    }
    static func dismantleUIView(_ view: MenuControlAnchorView, coordinator: ()) { view.activate = nil; view.adjust = nil }
    func updateUIView(_ view: MenuControlAnchorView, context: Context) {
        view.enabled = enabled; view.activate = activate; view.adjust = adjust
        view.accessibilityTraits = adjust == nil ? .button : .adjustable
    }
}

private struct ControllerPrimitiveStyle<Style: PrimitiveButtonStyle>: PrimitiveButtonStyle {
    let style: Style
    func makeBody(configuration: Configuration) -> some View {
        style.makeBody(configuration: configuration)
            .background(MenuControlAnchor(activate: { configuration.trigger() }))
    }
}

private struct ControllerAppearanceStyle<Style: ButtonStyle>: PrimitiveButtonStyle {
    let style: Style
    func makeBody(configuration: Configuration) -> some View {
        Button(configuration).buttonStyle(style)
            .background(MenuControlAnchor(activate: { configuration.trigger() }))
    }
}

extension View {
    func controllerButtonStyle<S: PrimitiveButtonStyle>(_ style: S) -> some View {
        buttonStyle(ControllerPrimitiveStyle(style: style))
    }
    func controllerButtonStyle<S: ButtonStyle>(_ style: S) -> some View {
        buttonStyle(ControllerAppearanceStyle(style: style))
    }
    func controllerAction(_ action: @escaping () -> Void) -> some View {
        background(MenuControlAnchor(activate: action))
    }
    func controllerToggle(_ value: Binding<Bool>) -> some View {
        background(MenuControlAnchor(activate: { value.wrappedValue.toggle() }))
    }
    func controllerAdjustable(_ value: Binding<Double>, range: ClosedRange<Double>, step: Double? = nil, onCommit: @escaping () -> Void = {}) -> some View {
        background(MenuControlAnchor(adjust: { direction in
            value.wrappedValue = min(range.upperBound, max(range.lowerBound, value.wrappedValue + Double(direction) * (step ?? (range.upperBound - range.lowerBound) / 20)))
            onCommit()
        }))
    }
}
