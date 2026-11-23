import Foundation

enum SaveExportLocation {
    static func make(title: String, titleID: UInt64, temporaryDirectory: URL = FileManager.default.temporaryDirectory) -> URL {
        let safeName = title.components(separatedBy: CharacterSet.alphanumerics.inverted)
            .filter { !$0.isEmpty }.joined(separator: "-").prefix(80)
        return temporaryDirectory.appendingPathComponent("77U-SaveExports", isDirectory: true)
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
            .appendingPathComponent("\(safeName.isEmpty ? "Wii-U-Save" : String(safeName))-\(String(format: "%016llX", titleID)).zip")
    }
}
