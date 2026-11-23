@main
struct ArtworkEnhancementLifecycleTests {
    static func main() {
        var lifecycle = ArtworkEnhancementLifecycle()
        precondition(lifecycle.canSchedule)

        lifecycle.prepareForGameLaunch()
        precondition(!lifecycle.canSchedule)
        lifecycle.userChangedPreference()
        precondition(!lifecycle.canSchedule, "Changing cover preference cannot start inference during gameplay")
        lifecycle.gameDidEnd()
        precondition(lifecycle.canSchedule, "Normal devices retain their existing post-game cover enhancement")

        lifecycle.receivedMemoryWarning()
        for _ in 0..<100 {
            precondition(!lifecycle.canSchedule, "Power and thermal callbacks must respect memory pressure")
        }
        lifecycle.prepareForGameLaunch()
        lifecycle.gameDidEnd()
        precondition(!lifecycle.canSchedule, "Returning to the library must not immediately reload the released model")
        lifecycle.userChangedPreference()
        precondition(lifecycle.canSchedule, "An explicit preference change allows a later retry")

        lifecycle.prepareForGameLaunch()
        lifecycle.receivedMemoryWarning()
        lifecycle.userChangedPreference()
        precondition(!lifecycle.canSchedule)
        lifecycle.gameDidEnd()
        precondition(lifecycle.canSchedule)

        print("PASS: artwork yields independently to gameplay and memory pressure; normal lifecycle and explicit retry remain available")
    }
}
