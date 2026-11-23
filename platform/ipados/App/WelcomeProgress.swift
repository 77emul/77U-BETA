import Foundation

/// Installation-local state: survives app updates, disappears when the app is deleted.
/// Excluded from backups so restoring app data does not silently skip the introduction.
enum WelcomeProgress {
    static var markerURL: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("77U/Welcome/completed-v1")
    }

    static func isComplete(at url: URL = markerURL) -> Bool {
        (try? Data(contentsOf: url)) == Data("77U-welcome-v1".utf8)
    }

    static func complete(at url: URL = markerURL) throws {
        var directory = url.deletingLastPathComponent()
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        try directory.setResourceValues(values)
        try Data("77U-welcome-v1".utf8).write(to: url, options: .atomic)
    }
}

enum WelcomeMemoryMessage {
    static func setupInstructions(inLiveContainer: Bool) -> String {
        if inLiveContainer {
            return "In GetMoreRAM, select the LiveContainer host's App ID and add Increased Memory Limit. Reinstall that LiveContainer through the same signing app while preserving its data, then fully close it and launch 77U again. Changing only 77U's guest IPA does not enable the host's memory capability."
        }
        return "In GetMoreRAM, select 77U's App ID and add Increased Memory Limit. Reinstall 77U through the same signing app, then reopen it to check the status."
    }

    static func activationStatus(entitlementPresent: Bool, statusKnown: Bool) -> String {
        guard statusKnown else { return "Unknown" }
        return entitlementPresent ? "Active" : "Inactive"
    }

    static func recommendsGetMoreRAM(physicalMemoryBytes: UInt64) -> Bool {
        physicalMemoryBytes <= 6 * 1_024 * 1_024 * 1_024
    }
}
