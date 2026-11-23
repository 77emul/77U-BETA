import SwiftUI
import UIKit
import CoreMotion
import GameController

enum TouchLayout: String, CaseIterable, Identifiable {
    case centered, edge, classic, arcade, compact
    var id: String { rawValue }
    var title: String { rawValue.capitalized }
    var detail: String {
        switch self {
        case .centered: return "Sticks inside the button rings"
        case .edge: return "Low, wide thumb reach"
        case .classic: return "Sticks above the buttons"
        case .arcade: return "D-pad first, larger actions"
        case .compact: return "More room for your game"
        }
    }
}

enum TouchButton: String, CaseIterable {
    case a = "A", b = "B", x = "X", y = "Y", l = "L", r = "R"
    case zl = "ZL", zr = "ZR", plus = "+", minus = "−", l3 = "L3", r3 = "R3"
    case up, down, left, right

    // Same physical positions as the existing AppleGCController default mapping.
    var bit: UInt64 {
        let index: Int
        switch self {
        case .a: index = 1
        case .b: index = 0
        case .x: index = 3
        case .y: index = 2
        case .minus: index = 4
        case .plus: index = 6
        case .l3: index = 7
        case .r3: index = 8
        case .l: index = 9
        case .r: index = 10
        case .up: index = 11
        case .down: index = 12
        case .left: index = 13
        case .right: index = 14
        case .zl: index = 32
        case .zr: index = 33
        }
        return UInt64(1) << index
    }
    var canLock: Bool { self == .zl || self == .zr }
}

/// One gesture has one outcome: hold, latch, or consume a tap to unlock.
/// Cancelling a gesture must never create a latch.
struct TriggerGesture {
    private(set) var locked = false
    private(set) var held = false
    private(set) var consumesUnlock = false

    mutating func begin() {
        consumesUnlock = locked
        locked = false
        held = !consumesUnlock
    }
    mutating func move(dx: CGFloat, dy: CGFloat) {
        guard !consumesUnlock else { return }
        if abs(dy) >= 28 && abs(dy) > abs(dx) { locked = true; held = true }
    }
    mutating func end() { held = locked; consumesUnlock = false }
    mutating func reset() { self = TriggerGesture() }
    mutating func setLocked(_ value: Bool) { locked = value; held = value }
}

/// Core Motion remains in the device's portrait coordinates. UIKit's landscape
/// interface orientations are opposite to UIDeviceOrientation's landscape cases.
/// Rotate gyro and acceleration together to preserve the frame's handedness.
enum TouchMotionCoordinates {
    static func screenVector(_ x: Double, _ y: Double, _ z: Double,
                             orientation: UIInterfaceOrientation) -> (Float, Float, Float) {
        switch orientation {
        case .landscapeLeft: return (Float(y), Float(-x), Float(z))
        case .landscapeRight: return (Float(-y), Float(x), Float(z))
        case .portraitUpsideDown: return (Float(-x), Float(-y), Float(z))
        default: return (Float(x), Float(y), Float(z))
        }
    }
}

@MainActor
final class TouchInputSession: ObservableObject {
    @Published var lockedTriggers: Set<TouchButton> = []
    @Published var resetGeneration = 0
    @Published var controllerConnected = false
    @Published private(set) var suppressesAutomaticDeviceMotion = false
    @Published var motionStatus = "Off"
    @Published var lastInput = "No buttons held"
    let motionManager = CMMotionManager()
    private var lastMotionTime: TimeInterval = 0
    private var motionOrientation: UIInterfaceOrientation?
    private var enabled = false
    private var gyroWanted = false
    private var sensitivity: Float = 1
    private let controllerPresence: () -> Bool

    init(controllerPresence: @escaping () -> Bool = {
        (GCController.current != nil && (GCController.current?.extendedGamepad != nil || GCController.current?.microGamepad != nil)) ||
        GCController.controllers().contains { $0.extendedGamepad != nil || $0.microGamepad != nil }
    }) {
        self.controllerPresence = controllerPresence
    }

    func refreshController() {
        let connected = controllerPresence()
        guard connected != controllerConnected else { return }
        if controllerConnected && !connected {
            // Substituting the iPad sensor for a controller mid-frame feeds a
            // different gravity frame into the Wii U filter and caused the
            // camera to creep upward. Stay neutral until a deliberate recenter.
            suppressesAutomaticDeviceMotion = true
            resetMotionSourceForControllerDisconnect()
        } else if connected {
            suppressesAutomaticDeviceMotion = false
            CemuBridge_RecenterControllerMotion()
        }
        controllerConnected = connected
        releaseAll()
    }

    /// A physical controller can disappear between two motion samples. Clear
    /// both motion providers before SwiftUI enables the iPad fallback so the
    /// emulated GamePad never receives the controller's stale final attitude.
    private func resetMotionSourceForControllerDisconnect() {
        gyroWanted = false
        motionManager.stopDeviceMotionUpdates()
        lastMotionTime = 0
        motionOrientation = nil
        CemuBridge_SetTouchMotionEnabled(false)
        CemuBridge_SetControllerMotionEnabled(false)
        CemuBridge_RecenterControllerMotion()
    }

    func setEnabled(_ value: Bool) {
        guard enabled != value else { return }
        releaseAll()
        enabled = value
        CemuBridge_SetTouchInputEnabled(value)
    }

    func send(buttons: UInt64, left: CGPoint, right: CGPoint, locks: Set<TouchButton>) {
        guard enabled else { return }
        CemuBridge_UpdateTouchInput(buttons, Float(left.x), Float(left.y), Float(right.x), Float(right.y))
        if locks != lockedTriggers { lockedTriggers = locks }
#if DEBUG
        let labels = TouchButton.allCases.filter { buttons & $0.bit != 0 }.map(\.rawValue)
        let summary = labels.isEmpty ? "No buttons held" : labels.joined(separator: " · ")
        if summary != lastInput { lastInput = summary }
#endif
    }

    func releaseAll() {
        CemuBridge_UpdateTouchInput(0, 0, 0, 0, 0)
        CemuBridge_UpdateDRCTouch(false, 0, 0)
        lockedTriggers = []
        lastInput = "No buttons held"
        resetGeneration &+= 1
    }

    func updateMotion(enabled: Bool, sensitivity: Double) {
        self.sensitivity = Float(sensitivity)
        gyroWanted = enabled
        guard enabled, motionManager.isDeviceMotionAvailable else {
            motionManager.stopDeviceMotionUpdates()
            lastMotionTime = 0
            motionOrientation = nil
            CemuBridge_SetTouchMotionEnabled(false)
            motionStatus = enabled ? "Unavailable on this device" : "Off"
            return
        }
        guard !motionManager.isDeviceMotionActive else { return }
        lastMotionTime = 0
        motionStatus = "Starting…"
        motionManager.deviceMotionUpdateInterval = 1.0 / 60.0
        motionManager.startDeviceMotionUpdates(using: .xArbitraryZVertical, to: .main) { [weak self] motion, error in
            MainActor.assumeIsolated {
                guard let self, self.gyroWanted else { return }
                guard let motion, error == nil else {
                    self.motionStatus = "Motion unavailable"
                    CemuBridge_SetTouchMotionEnabled(false)
                    return
                }
                // Dynamically resolve orientation for both iPhone and iPad.
                // iPhone supports portrait dual-deck mode and can transition between
                // portrait and landscape at runtime.
                let activeOrientation = UIApplication.shared.connectedScenes
                    .compactMap { $0 as? UIWindowScene }
                    .first(where: { $0.session.role == .windowApplication && $0.activationState == .foregroundActive })?
                    .effectiveGeometry.interfaceOrientation
                let fallback: UIInterfaceOrientation = UIDevice.current.userInterfaceIdiom == .phone ? .portrait : .landscapeLeft
                let orientation = (activeOrientation == nil || activeOrientation == .unknown) ? fallback : activeOrientation!
                if let previous = self.motionOrientation, previous != orientation {
                    // Discard fusion history from the previous screen coordinate frame.
                    CemuBridge_SetTouchMotionEnabled(false)
                    self.lastMotionTime = 0
                }
                self.motionOrientation = orientation
                let isPortrait = orientation.isPortrait
                let r = motion.rotationRate, a = motion.userAcceleration, g = motion.gravity
                let gyro = TouchMotionCoordinates.screenVector(r.x, r.y, r.z, orientation: orientation)
                let accel = TouchMotionCoordinates.screenVector(a.x + g.x, a.y + g.y, a.z + g.z, orientation: orientation)
                let dt = self.lastMotionTime == 0 ? 1.0 / 60.0 : motion.timestamp - self.lastMotionTime
                self.lastMotionTime = motion.timestamp
                CemuBridge_SetTouchMotionEnabled(true)
                CemuBridge_UpdateTouchMotion(Float(dt), gyro.0 * self.sensitivity, gyro.1 * self.sensitivity,
                                             gyro.2 * self.sensitivity, accel.0, accel.1, accel.2, isPortrait)
                if self.motionStatus != "Device motion active" { self.motionStatus = "Device motion active" }
            }
        }
    }

    func recenter() {
        suppressesAutomaticDeviceMotion = false
        CemuBridge_RecenterControllerMotion()
    }

    func stop() {
        setEnabled(false)
        releaseAll()
        updateMotion(enabled: false, sensitivity: Double(sensitivity))
    }
}

enum TouchControlKind: Equatable {
    case button(TouchButton), stick(Bool), dpad
}

struct TouchControlPlacement {
    let id: String
    let kind: TouchControlKind
    let frame: CGRect
}

struct TouchControlOffset: Codable, Equatable {
    var x: Double
    var y: Double
}

enum TouchControlGeometry {
    static let gridStep: CGFloat = 8

    static func minimumY(in size: CGSize, safeArea: UIEdgeInsets, phone: Bool,
                         externalDisplayMode: Bool = false) -> CGFloat {
        if externalDisplayMode { return size.height - min(max(size.height * 0.29, 210), 270) + 6 }
        if phone && size.height > size.width { return size.height - min(size.height * 0.44, 372) + 4 }
        return safeArea.top + (phone ? 46 : 58)
    }

    // The dots, default positions and drag snapping all describe control centers.
    static func positionedFrame(_ frame: CGRect, in size: CGSize, safeArea: UIEdgeInsets,
                                minimumY: CGFloat, snap: Bool, allowsLowerCorners: Bool = false) -> CGRect {
        func coordinate(_ value: CGFloat, lower: CGFloat, upper: CGFloat) -> CGFloat {
            if snap {
                let first = ceil(lower / gridStep) * gridStep
                let last = floor(upper / gridStep) * gridStep
                if first <= last { return min(max(first, (value / gridStep).rounded() * gridStep), last) }
            }
            return min(max(lower, value), upper)
        }
        let y = coordinate(frame.midY, lower: minimumY + frame.height / 2,
                           upper: size.height - max(safeArea.bottom, 4) - 4 - frame.height / 2)
        // Below the central camera area, use the extra horizontal space at the
        // lower corners. Upper controls retain the full safe-area inset.
        let lowerCorner = allowsLowerCorners && y - frame.height / 2 >= size.height * 0.65
        let left = lowerCorner ? max(16, safeArea.left - 32) : safeArea.left
        let right = lowerCorner ? max(16, safeArea.right - 32) : safeArea.right
        let x = coordinate(frame.midX, lower: left + 4 + frame.width / 2,
                           upper: size.width - right - 4 - frame.width / 2)
        return CGRect(x: x - frame.width / 2, y: y - frame.height / 2, width: frame.width, height: frame.height)
    }

    static func placements(in size: CGSize, safeArea: UIEdgeInsets, layout: TouchLayout,
                           scale: CGFloat, reach: CGFloat, spacing: CGFloat = 1,
                           offsets: [String: TouchControlOffset] = [:],
                           externalDisplayMode: Bool = false,
                           phone: Bool = false) -> [TouchControlPlacement] {
        if phone {
            return phonePlacements(in: size, safeArea: safeArea, layout: layout, scale: scale, reach: reach, spacing: spacing, offsets: offsets)
        }
        let availableWidth = size.width - safeArea.left - safeArea.right
        let deckHeight = min(max(size.height * 0.29, 210), 270)
        let availableHeight = externalDisplayMode ? deckHeight : size.height
        // The TV controller deck uses one stable size. A saved Compact preset
        // must not squeeze its hit targets or make neighboring controls overlap.
        let layoutScale: CGFloat = externalDisplayMode ? 1 : (layout == .compact ? 0.84 : 1)
        let s = min(scale * layoutScale, min(availableWidth / 720, availableHeight / (externalDisplayMode ? 230 : 490)))
        // Keep the default controls comfortably above the physical screen edge.
        // Custom offsets remain relative to this stable, device-sized baseline.
        let bottom = size.height - max(safeArea.bottom, 14) - (externalDisplayMode ? 8 : 32)
        let inset = 10 + reach * (size.height > size.width ? 22 : 38)
        let left = safeArea.left + inset
        let right = size.width - safeArea.right - inset
        var result: [TouchControlPlacement] = []
        func positioned() -> [TouchControlPlacement] {
            result.map { control in
                let offset = externalDisplayMode ? TouchControlOffset(x: 0, y: 0) : (offsets[control.id] ?? TouchControlOffset(x: 0, y: 0))
                let top = minimumY(in: size, safeArea: safeArea, phone: false, externalDisplayMode: externalDisplayMode)
                let base = positionedFrame(control.frame, in: size, safeArea: safeArea, minimumY: top, snap: true)
                let frame = positionedFrame(base.offsetBy(dx: offset.x * size.width, dy: offset.y * size.height),
                                            in: size, safeArea: safeArea, minimumY: top, snap: false)
                return TouchControlPlacement(id: control.id, kind: control.kind, frame: frame)
            }
        }
        func add(_ id: String, _ kind: TouchControlKind, _ x: CGFloat, _ fromBottom: CGFloat,
                 _ width: CGFloat, _ height: CGFloat) {
            let w = max(44, width * s), h = max(44, height * s)
            result.append(.init(id: id, kind: kind,
                                frame: CGRect(x: x - w / 2, y: bottom - fromBottom * s - h / 2, width: w, height: h)))
        }
        func button(_ button: TouchButton, _ x: CGFloat, _ y: CGFloat, _ w: CGFloat = 54, _ h: CGFloat = 54) {
            add(button.rawValue, .button(button), x, y, w, h)
        }
        if externalDisplayMode {
            // In TV mode the upper part of the iPad remains a real GamePad
            // touchscreen. Keep every physical control in a separate lower deck.
            let leftCenter = left + 132 * s
            let rightCenter = right - 132 * s
            let centerY: CGFloat = 80
            let spread: CGFloat = 58 * min(spacing, 1.15)
            add("leftStick", .stick(false), leftCenter, centerY, 64, 64)
            add("rightStick", .stick(true), rightCenter, centerY, 64, 64)
            button(.up, leftCenter, centerY + spread, 44, 44)
            button(.down, leftCenter, centerY - spread, 44, 44)
            button(.left, leftCenter - spread * s, centerY, 44, 44)
            button(.right, leftCenter + spread * s, centerY, 44, 44)
            button(.x, rightCenter, centerY + spread, 44, 44)
            button(.b, rightCenter, centerY - spread, 44, 44)
            button(.y, rightCenter - spread * s, centerY, 44, 44)
            button(.a, rightCenter + spread * s, centerY, 44, 44)
            button(.zl, left + 45 * s, 184, 88, 46)
            button(.l, left + 220 * s, 184, 76, 44)
            button(.zr, right - 45 * s, 184, 88, 46)
            button(.r, right - 220 * s, 184, 76, 44)
            button(.minus, size.width / 2 - 30, 146, 44, 44)
            button(.plus, size.width / 2 + 30, 146, 44, 44)
            button(.l3, left + 238 * s, 62, 44, 44)
            button(.r3, right - 238 * s, 62, 44, 44)
            return positioned()
        }
        if layout == .centered {
            let btnW: CGFloat = 56
            let spread = ceil(max(48 * spacing * s, (max(44, 56 * s) + max(44, 56 * s)) / 2 + 6) / gridStep) * gridStep / s
            let clusterInset = spread * s + max(44, btnW * s) / 2 + 4
            let dpadCenter = ceil((left + clusterInset) / gridStep) * gridStep
            let abxyCenter = floor((right - clusterInset) / gridStep) * gridStep
            let centerY: CGFloat = 112

            // D-Pad on outer left
            button(.up, dpadCenter, centerY + spread, btnW, btnW)
            button(.down, dpadCenter, centerY - spread, btnW, btnW)
            button(.left, dpadCenter - spread * s, centerY, btnW, btnW)
            button(.right, dpadCenter + spread * s, centerY, btnW, btnW)

            // Left Stick to the right of D-pad
            let stickOffset = spread * s + (max(44, btnW * s) + max(44, 72 * s)) / 2 + 14 * s
            let midX = size.width / 2
            let utilOffset = (max(44, 72 * s) + max(44, 44 * s)) / 2 + 10 * s
            let leftStickX = ceil(min(midX - 32 - utilOffset, dpadCenter + stickOffset) / gridStep) * gridStep
            let stickY: CGFloat = centerY - 6 / s
            add("leftStick", .stick(false), leftStickX, stickY, 72, 72)

            // Minus and L3
            let minusX = ceil(min(midX - 32, leftStickX + utilOffset) / gridStep) * gridStep
            let minusY = centerY + 58 / s
            button(.minus, minusX, minusY, 44, 44)

            let l3X = ceil(min(midX - 32, leftStickX + utilOffset) / gridStep) * gridStep
            let l3Y = centerY - 58 / s
            button(.l3, l3X, l3Y, 44, 44)

            // ABXY Diamond on outer right
            button(.x, abxyCenter, centerY + spread, btnW, btnW)
            button(.b, abxyCenter, centerY - spread, btnW, btnW)
            button(.y, abxyCenter - spread * s, centerY, btnW, btnW)
            button(.a, abxyCenter + spread * s, centerY, btnW, btnW)

            // Right Stick to the left of ABXY
            let rightStickX = floor(max(midX + 32 + utilOffset, abxyCenter - stickOffset) / gridStep) * gridStep
            add("rightStick", .stick(true), rightStickX, stickY, 72, 72)

            let plusX = floor(max(midX + 32, rightStickX - utilOffset) / gridStep) * gridStep
            button(.plus, plusX, minusY, 44, 44)

            let r3X = floor(max(midX + 32, rightStickX - utilOffset) / gridStep) * gridStep
            button(.r3, r3X, l3Y, 44, 44)

            // Triggers: comfortable thumb/finger height, not too high
            let triggerY = centerY + spread + ((max(44, btnW * s) + max(44, 50 * s)) / 2 + 12) / s
            button(.zl, left + 54 * s, triggerY, 98, 50)
            button(.l, left + 168 * s, triggerY, 86, 46)
            button(.zr, right - 54 * s, triggerY, 98, 50)
            button(.r, right - 168 * s, triggerY, 86, 46)

            return positioned()
        }
        let classic = layout == .classic
        let arcade = layout == .arcade
        add("leftStick", .stick(false), left + (arcade ? 214 : 66) * s, classic ? 220 : (arcade ? 68 : 76), 116, 116)
        add("dpad", .dpad, left + (arcade ? 70 : (classic ? 72 : 210)) * s,
            classic ? 68 : (arcade ? 93 : 78), 118, 118)
        add("rightStick", .stick(true), right - (classic ? 68 : 216) * s, classic ? 220 : 68, 108, 108)
        let cx = right - (classic ? 77 : 76) * s
        let cy: CGFloat = classic ? 72 : 98
        let actionSize: CGFloat = arcade ? 62 : 54
        let spread: CGFloat = (arcade ? 48 : 42) * spacing
        button(.x, cx, cy + spread, actionSize, actionSize)
        button(.a, cx + spread * s, cy, actionSize, actionSize)
        button(.b, cx, cy - spread, actionSize, actionSize)
        button(.y, cx - spread * s, cy, actionSize, actionSize)
        let triggerY: CGFloat = classic ? 315 : 235
        button(.zl, left + 54 * s, triggerY, 98, 50)
        button(.l, left + 166 * s, triggerY, 86, 46)
        button(.zr, right - 54 * s, triggerY, 98, 50)
        button(.r, right - 166 * s, triggerY, 86, 46)
        button(.l3, left + (arcade ? 214 : (classic ? 178 : 66)) * s, classic ? 150 : 166, 44, 44)
        button(.r3, right - (classic ? 178 : 216) * s, classic ? 150 : 155, 44, 44)
        // System buttons sit above the thumb groups on narrow windows.
        let systemY: CGFloat = availableWidth < 760 ? (classic ? 380 : 300) : 25
        button(.minus, size.width / 2 - 29, systemY, 46, 44)
        button(.plus, size.width / 2 + 29, systemY, 46, 44)
        return positioned()
    }

    // Phone layout: Uses the exact same centered iPad layout geometry (media_1790091807069.png),
    // proportionally scaled for iPhone screens in both landscape and portrait orientations.
    static func phonePlacements(in size: CGSize, safeArea: UIEdgeInsets, layout: TouchLayout,
                                scale: CGFloat, reach: CGFloat, spacing: CGFloat = 1,
                                offsets: [String: TouchControlOffset] = [:]) -> [TouchControlPlacement] {
        let portrait = size.height > size.width
        var result: [TouchControlPlacement] = []

        if portrait {
            // MARK: - iPhone Portrait Controller Deck (Bottom ~360pt)
            let availableWidth = size.width - safeArea.left - safeArea.right
            let deckHeight = min(size.height * 0.44, 372)
            let bottom = size.height - max(safeArea.bottom, 12) - 4
            let layoutScale: CGFloat = layout == .compact ? 0.84 : 1.0
            let s = min(scale * layoutScale, min(availableWidth / 390, deckHeight / 360)) * 0.78

            func add(_ id: String, _ kind: TouchControlKind, _ x: CGFloat, _ fromBottom: CGFloat,
                     _ width: CGFloat, _ height: CGFloat) {
                let minimum: CGFloat = if case .stick = kind { 56 } else { 44 }
                let w = max(minimum, width * s), h = max(minimum, height * s)
                result.append(.init(id: id, kind: kind,
                                    frame: CGRect(x: x - w / 2, y: bottom - fromBottom * s - h / 2, width: w, height: h)))
            }
            func button(_ button: TouchButton, _ x: CGFloat, _ y: CGFloat, _ w: CGFloat = 54, _ h: CGFloat = 54) {
                add(button.rawValue, .button(button), x, y, w, h)
            }

            let spread = ceil(max(52 * spacing * s, (max(56, 68 * s) + max(44, 50 * s)) / 2 + 8) / gridStep) * gridStep / s
            let clusterInset = spread * s + max(44, 50 * s) / 2 + 4
            let leftCenter = ceil((safeArea.left + clusterInset) / gridStep) * gridStep
            let rightCenter = floor((size.width - safeArea.right - clusterInset) / gridStep) * gridStep
            let centerY = (spread * s + max(44, 50 * s) / 2 + 4) / s

            // Left Cluster (Stick centered inside D-pad arrows)
            add("leftStick", .stick(false), leftCenter, centerY, 68, 68)
            button(.up, leftCenter, centerY + spread, 50, 50)
            button(.down, leftCenter, centerY - spread, 50, 50)
            button(.left, leftCenter - spread * s, centerY, 50, 50)
            button(.right, leftCenter + spread * s, centerY, 50, 50)

            // Right Cluster (Stick centered inside Action diamond)
            add("rightStick", .stick(true), rightCenter, centerY, 68, 68)
            button(.x, rightCenter, centerY + spread, 50, 50)
            button(.b, rightCenter, centerY - spread, 50, 50)
            button(.y, rightCenter - spread * s, centerY, 50, 50)
            button(.a, rightCenter + spread * s, centerY, 50, 50)

            // Triggers (Pills above each cluster)
            let triggerY = centerY + spread + ((max(44, 50 * s) + max(44, 42 * s)) / 2 + 8) / s
            let triggerGap = ceil(((max(44, 68 * s) + max(44, 64 * s)) / 2 + 8) / (gridStep * 2)) * gridStep * 2
            button(.zl, leftCenter - triggerGap / 2, triggerY, 68, 42)
            button(.l, leftCenter + triggerGap / 2, triggerY, 64, 42)
            button(.r, rightCenter - triggerGap / 2, triggerY, 64, 42)
            button(.zr, rightCenter + triggerGap / 2, triggerY, 68, 42)

            // Center Utility Buttons
            let midX = size.width * 0.50
            button(.minus, midX - 26, centerY + 56 / s, 44, 44)
            button(.plus, midX + 26, centerY + 56 / s, 44, 44)
            button(.l3, midX - 26, centerY - 56 / s, 44, 44)
            button(.r3, midX + 26, centerY - 56 / s, 44, 44)

            return result.map { control in
                let offset = offsets[control.id] ?? TouchControlOffset(x: 0, y: 0)
                let top = minimumY(in: size, safeArea: safeArea, phone: true)
                let base = positionedFrame(control.frame, in: size, safeArea: safeArea, minimumY: top, snap: true)
                let frame = positionedFrame(base.offsetBy(dx: offset.x * size.width, dy: offset.y * size.height),
                                            in: size, safeArea: safeArea, minimumY: top, snap: false)
                return TouchControlPlacement(id: control.id, kind: control.kind, frame: frame)
            }
        } else {
            // MARK: - iPhone Landscape (Exact iPad Centered Layout, media_1790091807069.png)
            let availableWidth = size.width - safeArea.left - safeArea.right
            let availableHeight = size.height - safeArea.top - safeArea.bottom
            let layoutScale: CGFloat = layout == .compact ? 0.84 : 1.0
            let s = min(scale * layoutScale, min(availableWidth / 640, availableHeight / 330)) * 0.86
            let bottom = size.height - max(safeArea.bottom, 12) - (layout == .centered ? 4 : 18)
            let inset = (layout == .centered ? 0 : 8) + reach * 30
            let left = safeArea.left + inset
            let right = size.width - safeArea.right - inset

            func add(_ id: String, _ kind: TouchControlKind, _ x: CGFloat, _ fromBottom: CGFloat,
                     _ width: CGFloat, _ height: CGFloat) {
                let minimum: CGFloat = if case .stick = kind { 56 } else { 44 }
                let w = max(minimum, width * s), h = max(minimum, height * s)
                result.append(.init(id: id, kind: kind,
                                    frame: CGRect(x: x - w / 2, y: bottom - fromBottom * s - h / 2, width: w, height: h)))
            }
            func button(_ button: TouchButton, _ x: CGFloat, _ y: CGFloat, _ w: CGFloat = 54, _ h: CGFloat = 54) {
                add(button.rawValue, .button(button), x, y, w, h)
            }

            if layout == .centered {
                let btnW: CGFloat = 56
                let spread = ceil(max(44 * spacing * s, (max(44, btnW * s) + max(44, btnW * s)) / 2 + 6) / gridStep) * gridStep / s
                let clusterInset = spread * s + max(44, btnW * s) / 2 + 4
                let centerY = spread + (max(44, btnW * s) / 2 + 4) / s
                let rowY = ((bottom - centerY * s) / gridStep).rounded() * gridStep
                let fitsLowerCorners = rowY - max(44, btnW * s) / 2 >= size.height * 0.65
                let ringLeft = (fitsLowerCorners ? max(16, safeArea.left - 32) : safeArea.left) + inset
                let ringRight = size.width - (fitsLowerCorners ? max(16, safeArea.right - 32) : safeArea.right) - inset
                let dpadCenter = ceil((ringLeft + clusterInset) / gridStep) * gridStep
                let abxyCenter = floor((ringRight - clusterInset) / gridStep) * gridStep

                // Left Cluster: D-Pad
                button(.up, dpadCenter, centerY + spread, btnW, btnW)
                button(.down, dpadCenter, centerY - spread, btnW, btnW)
                button(.left, dpadCenter - spread * s, centerY, btnW, btnW)
                button(.right, dpadCenter + spread * s, centerY, btnW, btnW)

                // Left Stick (to right of D-pad)
                let stickOffset = spread * s + (max(44, btnW * s) + max(56, 72 * s)) / 2 + 14 * s
                let leftStickX = ceil((dpadCenter + stickOffset) / gridStep) * gridStep
                let stickY = centerY - 8 / s
                add("leftStick", .stick(false), leftStickX, stickY, 72, 72)

                // Minus & L3
                let midX = size.width / 2
                let minusX = ceil(min(midX - 28, leftStickX + 36 * s + 24 * s) / gridStep) * gridStep
                let minusY = centerY + 70 / s
                button(.minus, minusX, minusY, 44, 44)

                let l3X = ceil(min(midX - 28, leftStickX + 36 * s + max(44, 44 * s) / 2 + 12 * s) / gridStep) * gridStep
                let l3Y = centerY - 8 / s
                button(.l3, l3X, l3Y, 44, 44)

                // Right Cluster: Action Diamond
                button(.x, abxyCenter, centerY + spread, btnW, btnW)
                button(.b, abxyCenter, centerY - spread, btnW, btnW)
                button(.y, abxyCenter - spread * s, centerY, btnW, btnW)
                button(.a, abxyCenter + spread * s, centerY, btnW, btnW)

                // Right Stick (to left of ABXY)
                let rightStickX = floor((abxyCenter - stickOffset) / gridStep) * gridStep
                add("rightStick", .stick(true), rightStickX, stickY, 72, 72)

                // Plus & R3
                let plusX = floor(max(midX + 28, right - (minusX - left)) / gridStep) * gridStep
                button(.plus, plusX, minusY, 44, 44)

                let r3X = floor(max(midX + 28, right - (l3X - left)) / gridStep) * gridStep
                button(.r3, r3X, l3Y, 44, 44)

                // Pill Triggers at Top Left / Top Right - comfortable height, not too high
                let triggerY = centerY + spread + ((max(44, btnW * s) + max(44, 46 * s)) / 2 + 12) / s
                button(.zl, left + 46 * s, triggerY, 86, 46)
                button(.l, left + 140 * s, triggerY, 78, 44)
                button(.zr, right - 46 * s, triggerY, 86, 46)
                button(.r, right - 140 * s, triggerY, 78, 44)
            } else {
                let classic = layout == .classic
                let arcade = layout == .arcade
                add("leftStick", .stick(false), left + (arcade ? 190 : 60) * s, classic ? 200 : (arcade ? 62 : 70), 108, 108)
                add("dpad", .dpad, left + (arcade ? 64 : (classic ? 66 : 190)) * s, classic ? 62 : (arcade ? 86 : 72), 110, 110)
                add("rightStick", .stick(true), right - (classic ? 62 : 196) * s, classic ? 200 : 62, 100, 100)
                let cx = right - (classic ? 70 : 70) * s
                let cy: CGFloat = classic ? 66 : 90
                let actionSize: CGFloat = arcade ? 56 : 50
                let spread: CGFloat = (arcade ? 44 : 38) * spacing
                button(.x, cx, cy + spread, actionSize, actionSize)
                button(.a, cx + spread * s, cy, actionSize, actionSize)
                button(.b, cx, cy - spread, actionSize, actionSize)
                button(.y, cx - spread * s, cy, actionSize, actionSize)
                let triggerY: CGFloat = min((size.height - safeArea.top - 58) / s, classic ? 285 : 215)
                button(.zl, left + 46 * s, triggerY, 86, 46)
                button(.l, left + 140 * s, triggerY, 78, 44)
                button(.zr, right - 46 * s, triggerY, 86, 46)
                button(.r, right - 140 * s, triggerY, 78, 44)
                button(.l3, left + (arcade ? 190 : (classic ? 160 : 60)) * s, classic ? 136 : 150, 44, 44)
                button(.r3, right - (classic ? 160 : 196) * s, classic ? 136 : 140, 44, 44)
                let systemY: CGFloat = (size.height - safeArea.top - 58) / s
                button(.minus, size.width / 2 - 28, systemY, 44, 44)
                button(.plus, size.width / 2 + 28, systemY, 44, 44)
            }

            return result.map { control in
                let offset = offsets[control.id] ?? TouchControlOffset(x: 0, y: 0)
                let top = minimumY(in: size, safeArea: safeArea, phone: true)
                let base = positionedFrame(control.frame, in: size, safeArea: safeArea, minimumY: top, snap: true,
                                           allowsLowerCorners: layout == .centered)
                let frame = positionedFrame(base.offsetBy(dx: offset.x * size.width, dy: offset.y * size.height),
                                            in: size, safeArea: safeArea, minimumY: top, snap: false,
                                            allowsLowerCorners: layout == .centered)
                return TouchControlPlacement(id: control.id, kind: control.kind, frame: frame)
            }
        }
    }

    static func axis(at point: CGPoint, in frame: CGRect, floatingOrigin: CGPoint? = nil, travelRadius: CGFloat? = nil) -> CGPoint {
        let origin = floatingOrigin ?? CGPoint(x: frame.midX, y: frame.midY)
        let radius = max(1, travelRadius ?? frame.width * 0.34)
        let x = (point.x - origin.x) / radius, y = (point.y - origin.y) / radius
        let magnitude = max(1, hypot(x, y))
        return CGPoint(x: x / magnitude, y: y / magnitude)
    }

    static func dpadButtons(at point: CGPoint, in frame: CGRect) -> UInt64 {
        let x = (point.x - frame.midX) / (frame.width / 2)
        let y = (point.y - frame.midY) / (frame.height / 2)
        guard hypot(x, y) > 0.2 else { return 0 }
        var mask: UInt64 = 0
        if x < -0.32 { mask |= TouchButton.left.bit }
        if x > 0.32 { mask |= TouchButton.right.bit }
        if y < -0.32 { mask |= TouchButton.up.bit }
        if y > 0.32 { mask |= TouchButton.down.bit }
        return mask
    }
}

/// Shared vector artwork, also exported as transparent PNGs by the preview harness.
/// No baked-in textures: the same marks remain sharp at every user-selected size.
@MainActor
enum TouchControlArtwork {
    static let ink = UIColor(white: 1, alpha: 0.94)

    static func draw(_ kind: TouchControlKind, in frame: CGRect, pressed: Bool = false,
                     locked: Bool = false, axis: CGPoint = .zero, opacity: CGFloat = 0.7,
                     stickTravel: CGFloat? = nil) {
        let context = UIGraphicsGetCurrentContext()!
        context.saveGState()
        defer { context.restoreGState() }
        let highlighted = pressed && !locked
        context.setAlpha(highlighted ? 1 : opacity)
        let rect = frame.insetBy(dx: 1.5, dy: 1.5)
        let foreground = highlighted ? UIColor(white: 0.08, alpha: 1) : ink
        let fill = highlighted ? UIColor(white: 0.96, alpha: 0.95) : UIColor(white: 0.055, alpha: 0.56)
        let border = UIColor(white: 1, alpha: highlighted ? 0.95 : 0.44)
        func shape(_ path: UIBezierPath) {
            fill.setFill(); path.fill()
            border.setStroke(); path.lineWidth = 1; path.stroke()
        }
        func text(_ string: String, center: CGPoint, size: CGFloat, color: UIColor? = nil) {
            let attrs: [NSAttributedString.Key: Any] = [.font: UIFont.systemFont(ofSize: size, weight: .medium), .foregroundColor: color ?? ink]
            let textSize = (string as NSString).size(withAttributes: attrs)
            (string as NSString).draw(at: CGPoint(x: center.x - textSize.width / 2, y: center.y - textSize.height / 2), withAttributes: attrs)
        }
        switch kind {
        case .button(let button):
            let pill = [.l, .r, .zl, .zr].contains(button)
            shape(UIBezierPath(roundedRect: rect, cornerRadius: pill ? 14 : rect.width / 2))
            if locked {
                UIImage(systemName: "lock.fill")?
                    .withTintColor(foreground, renderingMode: .alwaysOriginal)
                    .draw(in: CGRect(x: rect.midX - 8, y: rect.midY - 10, width: 16, height: 20))
            } else if [.up, .down, .left, .right].contains(button) {
                UIImage(systemName: "arrowtriangle.\(button.rawValue).fill")?
                    .withTintColor(foreground.withAlphaComponent(0.85), renderingMode: .alwaysOriginal)
                    .draw(in: CGRect(x: rect.midX - 8, y: rect.midY - 8, width: 16, height: 16))
            } else {
                text(button.rawValue, center: CGPoint(x: rect.midX, y: rect.midY),
                     size: [.l3, .r3].contains(button) ? 12 : (pill ? 16 : 20), color: foreground)
            }
        case .stick:
            if pressed, let travel = stickTravel {
                let halo = UIBezierPath(ovalIn: CGRect(x: rect.midX - travel, y: rect.midY - travel, width: travel * 2, height: travel * 2))
                UIColor.white.withAlphaComponent(0.18).setStroke(); halo.lineWidth = 1; halo.stroke()
            }
            let base = UIBezierPath(ovalIn: rect)
            UIColor(white: 0.055, alpha: 0.56).setFill(); base.fill()
            UIColor.white.withAlphaComponent(pressed ? 0.65 : 0.44).setStroke(); base.lineWidth = 1; base.stroke()
            let ring = UIBezierPath(ovalIn: rect.insetBy(dx: rect.width * 0.12, dy: rect.height * 0.12))
            UIColor.white.withAlphaComponent(0.09).setStroke(); ring.lineWidth = 1; ring.stroke()
            let diameter = rect.width * 0.48
            let knob = CGRect(x: rect.midX - diameter / 2 + axis.x * (stickTravel ?? rect.width * 0.25),
                              y: rect.midY - diameter / 2 + axis.y * (stickTravel ?? rect.height * 0.25),
                              width: diameter, height: diameter)
            (pressed ? UIColor(white: 1, alpha: 0.94) : UIColor(white: 1, alpha: 0.15)).setFill()
            UIBezierPath(ovalIn: knob).fill()
            UIColor.white.withAlphaComponent(0.48).setStroke(); UIBezierPath(ovalIn: knob).stroke()
        case .dpad:
            let third = rect.width / 3
            let path = UIBezierPath()
            let points = [CGPoint(x: third, y: 0), CGPoint(x: third * 2, y: 0), CGPoint(x: third * 2, y: third),
                          CGPoint(x: third * 3, y: third), CGPoint(x: third * 3, y: third * 2),
                          CGPoint(x: third * 2, y: third * 2), CGPoint(x: third * 2, y: third * 3),
                          CGPoint(x: third, y: third * 3), CGPoint(x: third, y: third * 2),
                          CGPoint(x: 0, y: third * 2), CGPoint(x: 0, y: third), CGPoint(x: third, y: third)]
            for (i, p) in points.enumerated() {
                let point = CGPoint(x: rect.minX + p.x, y: rect.minY + p.y)
                if i == 0 { path.move(to: point) } else { path.addLine(to: point) }
            }
            path.close(); path.lineJoinStyle = .round; shape(path)
            for (symbol, x, y) in [("chevron.up", 0.5, 0.17), ("chevron.down", 0.5, 0.83),
                                   ("chevron.left", 0.17, 0.5), ("chevron.right", 0.83, 0.5)] {
                UIImage(systemName: symbol)?.withTintColor(foreground.withAlphaComponent(0.6), renderingMode: .alwaysOriginal)
                    .draw(in: CGRect(x: rect.minX + rect.width * x - 5, y: rect.minY + rect.height * y - 5, width: 10, height: 10))
            }
        }
    }


}

@MainActor
final class TouchControllerView: UIView {
    var session: TouchInputSession?
    var layout: TouchLayout = .edge
    var controlScale: CGFloat = 1
    var controlOpacity: CGFloat = 0.7
    var reach: CGFloat = 0
    var floatingSticks = false
    var haptics = true
    var stickTravel: CGFloat = 72
    var spacing: CGFloat = 1
    var customOffsets: [String: TouchControlOffset] = [:]
    var externalDisplayMode = false
    var isEditing = false
    var snapToGrid = true
    var onEdit: (([String: TouchControlOffset]) -> Void)?
    private var editingControl: TouchControlPlacement?
    private var editingOrigin = CGPoint.zero
    private var artworkViews: [String: TouchArtworkView] = [:]
    var resetGeneration = -1
    private(set) var placements: [TouchControlPlacement] = []
    private var triggers: [TouchButton: TriggerGesture] = [.zl: TriggerGesture(), .zr: TriggerGesture()]
    private struct Tracking {
        var control: TouchControlPlacement
        let origin: CGPoint
        var point: CGPoint
        var inside = true
    }
    private var tracking: [UITouch: Tracking] = [:]
    private let feedback = UIImpactFeedbackGenerator(style: .soft)
    private var previousSize = CGSize.zero

    override init(frame: CGRect) {
        super.init(frame: frame)
        isOpaque = false
        backgroundColor = .clear
        isMultipleTouchEnabled = true
        isExclusiveTouch = false
        accessibilityIdentifier = "touch-controller"
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    func reset() {
        tracking.removeAll()
        triggers = [.zl: TriggerGesture(), .zr: TriggerGesture()]
        publish()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        if bounds.size != previousSize { previousSize = bounds.size; reset() }
        placements = TouchControlGeometry.placements(in: bounds.size, safeArea: safeAreaInsets, layout: layout,
                                                      scale: controlScale, reach: reach, spacing: spacing, offsets: customOffsets,
                                                      externalDisplayMode: externalDisplayMode, phone: traitCollection.userInterfaceIdiom == .phone)
        rebuildAccessibility()
        updateArtwork(animated: false)
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window == nil { reset() }
    }

    override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? {
        guard !isHidden, alpha > 0.01, isUserInteractionEnabled,
              placements.contains(where: { $0.frame.contains(point) }) else { return nil }
        return self
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        if isEditing, let touch = touches.first {
            editingOrigin = touch.location(in: self)
            editingControl = placements.first(where: { $0.frame.contains(editingOrigin) })
            updateArtwork(animated: true)
            return
        }
        for touch in touches {
            let point = touch.location(in: self)
            guard let control = placements.first(where: { $0.frame.contains(point) }),
                  !tracking.values.contains(where: { $0.control.id == control.id }) else { continue }
            tracking[touch] = Tracking(control: control, origin: point, point: point)
            if case .button(let button) = control.kind, button.canLock { triggers[button]?.begin() }
            if haptics { feedback.impactOccurred(intensity: 0.55) }
        }
        publish()
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        if isEditing, let selected = editingControl, let touch = touches.first {
            let point = touch.location(in: self)
            let phone = traitCollection.userInterfaceIdiom == .phone
            let frame = TouchControlGeometry.positionedFrame(
                selected.frame.offsetBy(dx: point.x - editingOrigin.x, dy: point.y - editingOrigin.y),
                in: bounds.size, safeArea: safeAreaInsets,
                minimumY: TouchControlGeometry.minimumY(in: bounds.size, safeArea: safeAreaInsets, phone: phone),
                snap: snapToGrid, allowsLowerCorners: phone && bounds.width > bounds.height && layout == .centered)
            let base = TouchControlGeometry.placements(in: bounds.size, safeArea: safeAreaInsets, layout: layout,
                                                       scale: controlScale, reach: reach, spacing: spacing, phone: traitCollection.userInterfaceIdiom == .phone)
            if let original = base.first(where: { $0.id == selected.id }) {
                customOffsets[selected.id] = TouchControlOffset(x: (frame.minX - original.frame.minX) / bounds.width,
                                                                 y: (frame.minY - original.frame.minY) / bounds.height)
            }
            setNeedsLayout(); layoutIfNeeded()
            return
        }
        for touch in touches {
            guard var track = tracking[touch] else { continue }
            track.point = touch.location(in: self)
            if case .button(let button) = track.control.kind {
                if button.canLock {
                    let wasLocked = triggers[button]?.locked == true
                    triggers[button]?.move(dx: track.point.x - track.origin.x, dy: track.point.y - track.origin.y)
                    if !wasLocked, triggers[button]?.locked == true, haptics { feedback.impactOccurred(intensity: 1) }
                } else {
                    track.inside = track.control.frame.insetBy(dx: -18, dy: -18).contains(track.point)
                }
            }
            tracking[touch] = track
        }
        publish()
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        if isEditing { editingControl = nil; onEdit?(customOffsets); updateArtwork(animated: true); return }
        for touch in touches {
            if let track = tracking.removeValue(forKey: touch), case .button(let button) = track.control.kind, button.canLock {
                triggers[button]?.end()
            }
        }
        publish()
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        if isEditing { editingControl = nil; onEdit?(customOffsets); updateArtwork(animated: false); return }
        for touch in touches {
            if let track = tracking.removeValue(forKey: touch), case .button(let button) = track.control.kind, button.canLock {
                triggers[button]?.reset()
            }
        }
        publish()
    }

    private func values() -> (UInt64, CGPoint, CGPoint, Set<TouchButton>) {
        var buttons: UInt64 = 0
        var left = CGPoint.zero, right = CGPoint.zero
        let locks = Set(triggers.filter { $0.value.locked }.map(\.key))
        for (button, state) in triggers where state.held { buttons |= button.bit }
        for track in tracking.values {
            switch track.control.kind {
            case .button(let button):
                if !button.canLock && track.inside { buttons |= button.bit }
            case .dpad: buttons |= TouchControlGeometry.dpadButtons(at: track.point, in: track.control.frame)
            case .stick(let isRight):
                let axis = TouchControlGeometry.axis(at: track.point, in: track.control.frame,
                                                    floatingOrigin: floatingSticks ? track.origin : nil, travelRadius: stickTravel)
                if isRight { right = axis } else { left = axis }
            }
        }
        return (buttons, left, right, locks)
    }

    private func publish() {
        let value = values()
        session?.send(buttons: value.0, left: value.1, right: value.2, locks: value.3)
        updateArtwork(animated: true, value: value)
    }

    private func updateArtwork(animated: Bool,
                               value suppliedValue: (UInt64, CGPoint, CGPoint, Set<TouchButton>)? = nil) {
        let value = suppliedValue ?? values()
        for placement in placements {
            let pressed: Bool
            var axis = CGPoint.zero, locked = false
            switch placement.kind {
            case .button(let button): pressed = value.0 & button.bit != 0; locked = value.3.contains(button)
            case .stick(let isRight): axis = isRight ? value.2 : value.1; pressed = axis != .zero
            case .dpad: pressed = value.0 & (TouchButton.up.bit | TouchButton.down.bit | TouchButton.left.bit | TouchButton.right.bit) != 0
            }
            let artwork: TouchArtworkView
            if let existing = artworkViews[placement.id] { artwork = existing }
            else {
                artwork = TouchArtworkView()
                artworkViews[placement.id] = artwork
                addSubview(artwork)
            }
            let isStick: Bool
            if case .stick = placement.kind { isStick = true } else { isStick = false }
            let padding = isStick ? stickTravel + 30 : CGFloat(8)
            let drawingFrame = placement.frame.insetBy(dx: -padding, dy: -padding)
            // Keep geometry independent of the presentation scale used for press feedback.
            artwork.bounds = CGRect(origin: .zero, size: drawingFrame.size)
            artwork.center = CGPoint(x: drawingFrame.midX, y: drawingFrame.midY)
            var controlRect = CGRect(x: padding, y: padding, width: placement.frame.width, height: placement.frame.height)
            if isStick, floatingSticks, let touch = tracking.values.first(where: { $0.control.id == placement.id }) {
                controlRect = controlRect.offsetBy(dx: touch.origin.x - placement.frame.midX, dy: touch.origin.y - placement.frame.midY)
            }
            artwork.controlRect = controlRect
            artwork.kind = placement.kind
            artwork.stickTravel = stickTravel
            artwork.controlOpacity = isEditing ? 1 : controlOpacity
            artwork.update(pressed: pressed, locked: locked, axis: axis, selected: editingControl?.id == placement.id,
                           animated: animated && !UIAccessibility.isReduceMotionEnabled)
            if isStick && axis != .zero { bringSubviewToFront(artwork) }
        }
        let valid = Set(placements.map(\.id))
        for id in Array(artworkViews.keys) where !valid.contains(id) {
            artworkViews.removeValue(forKey: id)?.removeFromSuperview()
        }
    }

    private func rebuildAccessibility() {
        accessibilityElements = placements.map { placement in
            let element = UIAccessibilityElement(accessibilityContainer: self)
            element.accessibilityLabel = placement.id == "dpad" ? "Directional pad" : placement.id
            if case .button = placement.kind { element.accessibilityTraits = [.button, .allowsDirectInteraction] }
            else { element.accessibilityTraits = .allowsDirectInteraction }
            element.accessibilityFrameInContainerSpace = placement.frame
            if case .button(let button) = placement.kind, button.canLock {
                element.accessibilityHint = "Swipe up or down to lock. Tap again to release."
                element.accessibilityCustomActions = [UIAccessibilityCustomAction(name: "Toggle trigger lock", actionHandler: { [weak self] _ in
                    guard let self else { return false }
                    let locked = self.triggers[button]?.locked == true
                    self.triggers[button]?.setLocked(!locked)
                    self.publish()
                    return true
                })]
            }
            return element
        }
    }
}

struct TouchControllerSurface: UIViewRepresentable {
    @ObservedObject var session: TouchInputSession
    var layout: TouchLayout
    var scale: Double
    var opacity: Double
    var reach: Double
    var floatingSticks: Bool
    var haptics: Bool
    var spacing: Double = 1
    var stickTravel: Double = 72
    var offsets: [String: TouchControlOffset] = [:]
    var externalDisplayMode = false

    func makeUIView(context: Context) -> TouchControllerView {
        let view = TouchControllerView()
        view.session = session
        return view
    }
    func updateUIView(_ view: TouchControllerView, context: Context) {
        view.session = session
        view.layout = layout
        view.controlScale = scale
        view.controlOpacity = opacity
        view.reach = reach
        view.floatingSticks = floatingSticks
        view.haptics = haptics
        view.spacing = spacing
        view.stickTravel = stickTravel
        view.customOffsets = offsets
        view.externalDisplayMode = externalDisplayMode
        if view.resetGeneration != session.resetGeneration {
            view.resetGeneration = session.resetGeneration
            // State has already been cleared by the owner. Avoid publishing during a SwiftUI update.
            view.session = nil; view.reset(); view.session = session
        }
        view.setNeedsLayout()
        view.setNeedsDisplay()
    }
    static func dismantleUIView(_ view: TouchControllerView, coordinator: ()) { view.session = nil; view.reset() }
}

/// Core Animation handles the short press response on the compositor. There is
/// no display link, repeating animation, or timer competing with game rendering.
@MainActor
private final class TouchArtworkView: UIView {
    var kind = TouchControlKind.dpad { didSet { if kind != oldValue { setNeedsDisplay() } } }
    var controlRect = CGRect.zero { didSet { if controlRect != oldValue { setNeedsDisplay() } } }
    var controlOpacity: CGFloat = 0.7 { didSet { if controlOpacity != oldValue { setNeedsDisplay() } } }
    var stickTravel: CGFloat = 72 { didSet { if stickTravel != oldValue { setNeedsDisplay() } } }
    private var pressed = false
    private var locked = false
    private var axis = CGPoint.zero
    private var selected = false

    override init(frame: CGRect) {
        super.init(frame: frame)
        isOpaque = false
        isUserInteractionEnabled = false
        backgroundColor = .clear
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    func update(pressed: Bool, locked: Bool, axis: CGPoint, selected: Bool, animated: Bool) {
        let changed = pressed != self.pressed || selected != self.selected
        let lockChanged = locked != self.locked
        let axisChanged = axis != self.axis
        self.pressed = pressed; self.locked = locked; self.axis = axis; self.selected = selected
        if changed {
            let isStick: Bool
            if case .stick = kind { isStick = true } else { isStick = false }
            let target: CGFloat = selected ? 1.035 : (pressed && !isStick ? 0.95 : 1)
            let current = (layer.presentation()?.value(forKeyPath: "transform.scale") as? CGFloat) ?? 1
            CATransaction.begin(); CATransaction.setDisableActions(true)
            layer.transform = CATransform3DMakeScale(target, target, 1)
            CATransaction.commit()
            if animated {
                let spring = CASpringAnimation(keyPath: "transform.scale")
                spring.fromValue = current; spring.toValue = target
                spring.mass = 1; spring.stiffness = 420; spring.damping = 36
                spring.duration = min(0.28, spring.settlingDuration)
                layer.add(spring, forKey: "press")
            }
        }
        if lockChanged && animated {
            let fade = CATransition(); fade.type = .fade; fade.duration = 0.16
            layer.add(fade, forKey: "lock")
        }
        if changed || lockChanged || axisChanged { setNeedsDisplay() }
    }

    override func draw(_ rect: CGRect) {
        TouchControlArtwork.draw(kind, in: controlRect, pressed: pressed, locked: locked,
                                 axis: axis, opacity: controlOpacity, stickTravel: stickTravel)
        if selected {
            let outline = UIBezierPath(roundedRect: controlRect.insetBy(dx: -5, dy: -5), cornerRadius: 16)
            outline.setLineDash([3, 4], count: 2, phase: 0)
            UIColor.white.withAlphaComponent(0.8).setStroke(); outline.lineWidth = 1; outline.stroke()
        }
    }
}

struct TouchLayoutEditor: View {
    @ObservedObject var preferences: PlayPreferences
    let layout: TouchLayout
    @Environment(\.dismiss) private var dismiss
    @State private var draft: [String: TouchControlOffset]
    @State private var snap = true
    @State private var resetGeneration = 0

    init(preferences: PlayPreferences, layout: TouchLayout) {
        self.preferences = preferences
        self.layout = layout
        _draft = State(initialValue: preferences.configuration.customPositions[layout.rawValue] ?? [:])
    }

    var body: some View {
        ZStack {
            Color(uiColor: .systemBackground).ignoresSafeArea()
            Canvas { context, size in
                if snap {
                    var dots = Path()
                    for x in stride(from: TouchControlGeometry.gridStep, to: size.width, by: TouchControlGeometry.gridStep) {
                        for y in stride(from: TouchControlGeometry.gridStep, to: size.height, by: TouchControlGeometry.gridStep) {
                            dots.addEllipse(in: CGRect(x: x - 0.5, y: y - 0.5, width: 1, height: 1))
                        }
                    }
                    context.fill(dots, with: .color(.secondary.opacity(0.18)))
                }
            }.ignoresSafeArea().allowsHitTesting(false)
            EditableTouchSurface(configuration: preferences.configuration, layout: layout, offsets: $draft, snap: snap)
                .id(resetGeneration)
                .ignoresSafeArea()
            VStack(spacing: 8) {
                HStack {
                    Button("Cancel") { dismiss() }.modifier(NativeGlassButton())
                    Spacer()
                    Text("Edit controls").font(.headline)
                    Spacer()
                    Button("Save") {
                        preferences.configuration.customPositions[layout.rawValue] = draft
                        dismiss()
                    }.modifier(NativeGlassButton())
                }.controlSize(.large)
                Text("Drag a button or stick to move it.")
                    .font(.caption).foregroundStyle(.secondary).allowsHitTesting(false)
                Spacer()
                LibraryGlassGroup {
                    HStack(spacing: 12) {
                        Button("Reset positions", systemImage: "arrow.counterclockwise") {
                            draft = [:]
                            resetGeneration += 1
                        }
                        .labelStyle(.iconOnly).modifier(NativeGlassButton())
                        Button("Snap to grid", systemImage: "grid") { snap.toggle() }
                            .modifier(NativeGlassButton(prominent: snap))
                            .accessibilityValue(snap ? "On" : "Off")
                    }.controlSize(.large)
                }
            }
            .padding(.horizontal, 20).padding(.vertical, 8)
        }
        .preferredColorScheme(.dark)
        .statusBarHidden()
    }

}

private struct EditableTouchSurface: UIViewRepresentable {
    let configuration: PlayConfiguration
    let layout: TouchLayout
    @Binding var offsets: [String: TouchControlOffset]
    let snap: Bool

    func makeUIView(context: Context) -> TouchControllerView {
        let view = TouchControllerView()
        view.isEditing = true
        return view
    }
    func updateUIView(_ view: TouchControllerView, context: Context) {
        view.layout = layout
        view.controlScale = configuration.scale
        view.reach = configuration.reach
        view.spacing = configuration.buttonSpacing
        view.stickTravel = configuration.stickTravel
        view.customOffsets = offsets
        view.snapToGrid = snap
        view.onEdit = { offsets = $0 }
        view.setNeedsLayout()
    }
}
