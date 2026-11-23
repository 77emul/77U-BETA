import Foundation
#if os(macOS)
import Darwin
#endif

// Run with swiftc App/WelcomeProgress.swift Tests/WelcomeProgressTests.swift -o /tmp/welcome-tests
@main struct WelcomeProgressTests {
    static func main() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let first = root.appendingPathComponent("installation-one/Welcome/completed-v1")
        let second = root.appendingPathComponent("installation-two/Welcome/completed-v1")
        precondition(!WelcomeProgress.isComplete(at: first), "A fresh installation must show the guide")
        try WelcomeProgress.complete(at: first)
        precondition(WelcomeProgress.isComplete(at: first), "Completion must survive another read/app launch")
        try WelcomeProgress.complete(at: first)
        precondition(WelcomeProgress.isComplete(at: first), "Repeated completion must be safe")
        #if os(macOS)
        // NSURLIsExcludedFromBackupKey may read false for an already-excluded volume;
        // check the actual Time Machine exclusion attribute written by Foundation.
        let directory = first.deletingLastPathComponent().path
        let attributeLength = directory.withCString {
            getxattr($0, "com.apple.metadata:com_apple_backup_excludeItem", nil, 0, 0, 0)
        }
        precondition(attributeLength > 0, "Completion directory must carry the backup exclusion")
        #else
        let values = try first.deletingLastPathComponent().resourceValues(forKeys: [.isExcludedFromBackupKey])
        precondition(values.isExcludedFromBackup == true, "Completion must not be restored from a backup")
        #endif
        precondition(!WelcomeProgress.isComplete(at: second), "A new app container must show the guide again")
        try Data("interrupted-write".utf8).write(to: first)
        precondition(!WelcomeProgress.isComplete(at: first), "Invalid completion data must not skip the guide")
        try FileManager.default.removeItem(at: first.deletingLastPathComponent())
        precondition(!WelcomeProgress.isComplete(at: first), "Deleting the installation data resets onboarding")
        print("PASS: fresh install, completion, relaunch, repeat, backup exclusion, reinstall, corrupt state, removal")
    }
}
