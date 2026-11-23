/// Optional cover inference must yield to title loading and real memory pressure.
/// Kept separate from the model and UI so lifecycle transitions can be tested.
struct ArtworkEnhancementLifecycle {
    private(set) var isGameActive = false
    private(set) var isUnderMemoryPressure = false

    var canSchedule: Bool { !isGameActive && !isUnderMemoryPressure }

    mutating func prepareForGameLaunch() {
        isGameActive = true
    }

    mutating func gameDidEnd() {
        isGameActive = false
    }

    mutating func receivedMemoryWarning() {
        isUnderMemoryPressure = true
    }

    mutating func userChangedPreference() {
        // An explicit user request may retry later; thermal/power notifications
        // and returning from gameplay must not restart work after a warning.
        isUnderMemoryPressure = false
    }
}
