import SwiftUI
import UIKit
import UniformTypeIdentifiers

private struct SaveInfoSnapshot: Equatable, Sendable {
    let available: Bool
    let saveExists: Bool
    let hasUserSaveData: Bool
    let rollbackExists: Bool
    let totalBytes: UInt64
    let modifiedUnixSeconds: Int64
    let rollbackName: String
    let error: String

    static let empty = SaveInfoSnapshot(
        available: false,
        saveExists: false,
        hasUserSaveData: false,
        rollbackExists: false,
        totalBytes: 0,
        modifiedUnixSeconds: 0,
        rollbackName: "",
        error: ""
    )
}

private struct SaveOperationSnapshot: Sendable {
    let success: Bool
    let outputPath: String
    let rollbackPath: String
    let error: String
}

private struct SaveNotice: Identifiable {
    let id = UUID()
    let title: String
    let message: String
}

@MainActor
private final class SaveManagementModel: ObservableObject {
    @Published private(set) var info = SaveInfoSnapshot.empty
    @Published private(set) var isBusy = false
    @Published private(set) var activityText = ""
    @Published var notice: SaveNotice?

    private let titleID: UInt64
    private let title: String

    init(titleID: UInt64, title: String) {
        self.titleID = titleID
        self.title = title
    }

    func refresh() async {
        guard titleID != 0 else {
            info = SaveInfoSnapshot(
                available: false,
                saveExists: false,
                hasUserSaveData: false,
                rollbackExists: false,
                totalBytes: 0,
                modifiedUnixSeconds: 0,
                rollbackName: "",
                error: "This game does not expose a valid Wii U title ID."
            )
            return
        }

        isBusy = true
        activityText = "Checking save data…"
        let id = titleID
        let snapshot = await Task.detached(priority: .userInitiated) {
            let result = CemuBridge_GetSaveInfo(id)
            let rollbackPath = cString(result.latestRollbackPath)
            return SaveInfoSnapshot(
                available: result.success,
                saveExists: result.saveExists,
                hasUserSaveData: result.hasUserSaveData,
                rollbackExists: result.rollbackExists,
                totalBytes: result.totalBytes,
                modifiedUnixSeconds: result.modifiedUnixSeconds,
                rollbackName: rollbackPath.isEmpty ? "" : URL(fileURLWithPath: rollbackPath).lastPathComponent,
                error: cString(result.errorMessage)
            )
        }.value
        info = snapshot
        isBusy = false
        activityText = ""
    }

    func exportSave() async -> URL? {
        guard titleID != 0, info.hasUserSaveData else { return nil }
        isBusy = true
        activityText = "Creating backup…"

        let id = titleID
        let gameTitle = title
        let outputURL = SaveExportLocation.make(title: title, titleID: id)

        let operation = await Task.detached(priority: .userInitiated) {
            do {
                try FileManager.default.createDirectory(
                    at: outputURL.deletingLastPathComponent(),
                    withIntermediateDirectories: true
                )
            } catch {
                return SaveOperationSnapshot(success: false, outputPath: "", rollbackPath: "", error: error.localizedDescription)
            }

            let result = gameTitle.withCString { titlePointer in
                outputURL.path.withCString { pathPointer in
                    CemuBridge_ExportSave(id, titlePointer, pathPointer)
                }
            }
            return SaveOperationSnapshot(
                success: result.success,
                outputPath: cString(result.outputPath),
                rollbackPath: cString(result.rollbackPath),
                error: cString(result.errorMessage)
            )
        }.value

        isBusy = false
        activityText = ""
        guard operation.success else {
            notice = SaveNotice(title: "Export failed", message: operation.error.isEmpty ? "The save could not be exported." : operation.error)
            return nil
        }
        return URL(fileURLWithPath: operation.outputPath)
    }

    func stageImport(from sourceURL: URL) async -> URL? {
        let hasSecurityScope = sourceURL.startAccessingSecurityScopedResource()
        isBusy = true
        activityText = "Preparing backup…"
        let destination = FileManager.default.temporaryDirectory
            .appendingPathComponent("77U-SaveImports", isDirectory: true)
            .appendingPathComponent("\(UUID().uuidString).zip")

        let copyError = await Task.detached(priority: .userInitiated) { () -> String? in
            do {
                try FileManager.default.createDirectory(
                    at: destination.deletingLastPathComponent(),
                    withIntermediateDirectories: true
                )
                try FileManager.default.copyItem(at: sourceURL, to: destination)
                return nil
            } catch {
                return error.localizedDescription
            }
        }.value
        if hasSecurityScope {
            sourceURL.stopAccessingSecurityScopedResource()
        }

        isBusy = false
        activityText = ""
        if let copyError {
            notice = SaveNotice(title: "Import failed", message: copyError)
            return nil
        }
        return destination
    }

    func restoreSave(from archiveURL: URL) async {
        guard titleID != 0 else { return }
        isBusy = true
        activityText = "Validating and restoring…"
        let id = titleID
        let operation = await Task.detached(priority: .userInitiated) {
            let result = archiveURL.path.withCString { CemuBridge_RestoreSave(id, $0) }
            try? FileManager.default.removeItem(at: archiveURL)
            return SaveOperationSnapshot(
                success: result.success,
                outputPath: cString(result.outputPath),
                rollbackPath: cString(result.rollbackPath),
                error: cString(result.errorMessage)
            )
        }.value

        isBusy = false
        activityText = ""
        if operation.success {
            let rollbackMessage = operation.rollbackPath.isEmpty
                ? "The backup was restored successfully."
                : "The backup was restored successfully. Your previous save was kept as an automatic rollback."
            notice = SaveNotice(title: "Save restored", message: rollbackMessage)
            await refresh()
        } else {
            notice = SaveNotice(title: "Restore failed", message: operation.error.isEmpty ? "The save could not be restored." : operation.error)
        }
    }

    func restoreRollback() async {
        guard titleID != 0, !isBusy, info.rollbackExists else { return }
        isBusy = true
        activityText = "Restoring previous save…"
        let id = titleID
        let operation = await Task.detached(priority: .userInitiated) {
            let result = CemuBridge_RestoreRollback(id)
            return SaveOperationSnapshot(success: result.success, outputPath: "", rollbackPath: cString(result.rollbackPath), error: cString(result.errorMessage))
        }.value
        isBusy = false
        activityText = ""
        await refresh()
        notice = operation.success
            ? SaveNotice(title: "Previous save restored", message: operation.rollbackPath.isEmpty ? "Your previous save is ready." : "Your previous save is ready. The save it replaced was kept as a rollback.")
            : SaveNotice(title: "Couldn't restore previous save", message: operation.error.isEmpty ? "The rollback could not be restored." : operation.error)
    }

    func deleteSave() async {
        guard titleID != 0, info.saveExists else { return }
        isBusy = true
        activityText = "Deleting save data…"
        let id = titleID
        let operation = await Task.detached(priority: .userInitiated) {
            let result = CemuBridge_DeleteSave(id)
            return SaveOperationSnapshot(
                success: result.success,
                outputPath: "",
                rollbackPath: "",
                error: cString(result.errorMessage)
            )
        }.value
        isBusy = false
        activityText = ""
        await refresh()
        notice = operation.success
            ? SaveNotice(title: "Save deleted", message: "The local save was deleted. Exported backups are kept.")
            : SaveNotice(title: "Delete failed", message: operation.error.isEmpty ? "The save could not be completely deleted." : operation.error)
    }

    func discardStagedArchive(_ archiveURL: URL?) {
        guard let archiveURL else { return }
        try? FileManager.default.removeItem(at: archiveURL)
    }
}

struct SaveManagementView: View {
    @Environment(\.dismiss) private var dismiss
    @StateObject private var model: SaveManagementModel
    @State private var isImporterPresented = false
    @State private var isRestoreConfirmationPresented = false
    @State private var isRollbackConfirmationPresented = false
    @State private var isDeleteConfirmationPresented = false
    @State private var isRebuildCacheConfirmationPresented = false
    @State private var pendingArchiveURL: URL?

    private let game: GameLibraryEntry

    init(game: GameLibraryEntry) {
        self.game = game
        _model = StateObject(wrappedValue: SaveManagementModel(
            titleID: game.numericTitleID ?? 0,
            title: game.title
        ))
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Label(game.title, systemImage: "externaldrive").font(.headline)
                }
                Section("Current save") {
                    if model.info.saveExists {
                        LabeledContent("Status", value: model.info.hasUserSaveData ? "Available" : "No game progress found")
                        LabeledContent("Size", value: ByteCountFormatter.string(fromByteCount: Int64(clamping: model.info.totalBytes), countStyle: .file))
                        if model.info.modifiedUnixSeconds > 0 {
                            LabeledContent(
                                "Last changed",
                                value: Date(timeIntervalSince1970: TimeInterval(model.info.modifiedUnixSeconds))
                                    .formatted(date: .abbreviated, time: .shortened)
                            )
                        }
                    } else if model.info.available {
                        LabeledContent("Status", value: "No save data yet")
                    } else if !model.info.error.isEmpty {
                        Text(model.info.error)
                            .foregroundStyle(.red)
                    }
                }

                Section {
                    Button("Export backup", systemImage: "square.and.arrow.up") {
                        Task {
                            if let archiveURL = await model.exportSave() {
                                presentShareSheet(for: archiveURL)
                            }
                        }
                    }
                    .disabled(model.isBusy || !model.info.hasUserSaveData)

                    Button("Import backup", systemImage: "square.and.arrow.down") {
                        isImporterPresented = true
                    }
                    .disabled(model.isBusy || game.numericTitleID == nil)

                    if model.info.rollbackExists {
                        Button("Restore previous save", systemImage: "clock.arrow.circlepath") {
                            isRollbackConfirmationPresented = true
                        }
                        .disabled(model.isBusy || !CemuBridge_CanManageSaves())
                    }
                } header: {
                    Text("Backups")
                } footer: {
                    Text("Importing a backup keeps your current save as a rollback.")
                }

                Section {
                    Button("Delete save", systemImage: "trash", role: .destructive) {
                        isDeleteConfirmationPresented = true
                    }
                    .disabled(model.isBusy || !model.info.saveExists)
                } header: {
                    Text("Delete")
                } footer: {
                    Text("Removes this game's local saves for all accounts. Exported backups and rollbacks are kept.")
                }

                Section {
                    Button("Clear shader cache", systemImage: "arrow.clockwise") {
                        isRebuildCacheConfirmationPresented = true
                    }
                    .disabled(model.isBusy || game.numericTitleID == nil || !CemuBridge_CanManageSaves())
                } header: {
                    Text("Shader cache")
                } footer: {
                    Text("Restart 77U after clearing. Shaders will compile again; saves are kept.")
                }

                if model.isBusy {
                    Section {
                        HStack(spacing: 12) {
                            ProgressView()
                            Text(model.activityText)
                        }
                    }
                }
            }
            .navigationTitle("Saves")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                        .disabled(model.isBusy)
                }
            }
        }
        .modifier(MenuSheetPresentation())
        .interactiveDismissDisabled(model.isBusy)
        .task {
            await model.refresh()
        }
        .fileImporter(isPresented: $isImporterPresented, allowedContentTypes: [.zip]) { result in
            switch result {
            case .success(let sourceURL):
                Task {
                    if let stagedURL = await model.stageImport(from: sourceURL) {
                        pendingArchiveURL = stagedURL
                        isRestoreConfirmationPresented = true
                    }
                }
            case .failure(let error):
                if (error as NSError).code != NSUserCancelledError { model.notice = SaveNotice(title: "Import failed", message: error.localizedDescription) }
            }
        }
        .confirmationDialog(
            "Replace the current save?",
            isPresented: $isRestoreConfirmationPresented,
            titleVisibility: .visible
        ) {
            Button("Restore backup", role: .destructive) {
                guard let archiveURL = pendingArchiveURL else { return }
                pendingArchiveURL = nil
                Task { await model.restoreSave(from: archiveURL) }
            }
            Button("Cancel", role: .cancel) {
                model.discardStagedArchive(pendingArchiveURL)
                pendingArchiveURL = nil
            }
        } message: {
            Text("77U will validate the backup and keep your current save as a rollback before replacing it.")
        }
        .confirmationDialog(
            "Restore the previous save?",
            isPresented: $isRollbackConfirmationPresented,
            titleVisibility: .visible
        ) {
            Button("Restore previous save", role: .destructive) {
                Task { await model.restoreRollback() }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("77U will restore the latest automatic rollback and keep your current save as another rollback.")
        }
        .confirmationDialog(
            "Permanently delete this game's save?",
            isPresented: $isDeleteConfirmationPresented,
            titleVisibility: .visible
        ) {
            Button("Delete save", role: .destructive) {
                Task { await model.deleteSave() }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("All local progress for this game and every Wii U account will be removed. This cannot be undone unless you have a backup. Exported backups and automatic rollbacks stay available.")
        }
        .confirmationDialog(
            "Clear shader caches for \(game.title)?",
            isPresented: $isRebuildCacheConfirmationPresented,
            titleVisibility: .visible
        ) {
            Button("Clear caches", role: .destructive) {
                if let titleID = game.numericTitleID {
                    let cleared = CemuBridge_ClearShaderCacheForTitle(titleID)
                    model.notice = SaveNotice(
                        title: cleared ? "Caches cleared" : "Couldn't clear caches",
                        message: cleared
                            ? "77U shader and pipeline records for \(game.title) were removed. Close and reopen 77U for a fresh shader test. Saved progress is preserved."
                            : "77U could not finish clearing these caches. Close and reopen 77U, then try again before starting a game."
                    )
                }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Shaders will compile again for \(game.title). Saves are kept.")
        }
        .alert(item: $model.notice) { notice in
            Alert(title: Text(notice.title), message: Text(notice.message), dismissButton: .default(Text("OK")))
        }
        .onChange(of: isRestoreConfirmationPresented) { _, presented in
            if !presented, let pendingArchiveURL {
                model.discardStagedArchive(pendingArchiveURL)
                self.pendingArchiveURL = nil
            }
        }
        .onDisappear { model.discardStagedArchive(pendingArchiveURL) }
    }

    private func presentShareSheet(for archiveURL: URL) {
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        guard let windowScene = scenes.first(where: {
            $0.session.role == .windowApplication &&
            ($0.activationState == .foregroundActive || $0.activationState == .foregroundInactive)
        }) ?? scenes.first,
        let rootViewController = windowScene.windows.first(where: { $0.isKeyWindow })?.rootViewController
            ?? windowScene.windows.first?.rootViewController else {
            model.notice = SaveNotice(title: "Backup ready", message: "The backup was created at \(archiveURL.lastPathComponent), but the share sheet could not be opened.")
            return
        }

        let activityViewController = UIActivityViewController(activityItems: [archiveURL], applicationActivities: nil)
        activityViewController.completionWithItemsHandler = { _, _, _, _ in
            try? FileManager.default.removeItem(at: archiveURL)
        }
        if let popover = activityViewController.popoverPresentationController {
            popover.sourceView = rootViewController.view
            popover.sourceRect = CGRect(x: rootViewController.view.bounds.midX, y: rootViewController.view.bounds.midY, width: 0, height: 0)
            popover.permittedArrowDirections = []
        }

        var presenter = rootViewController
        while let presented = presenter.presentedViewController {
            presenter = presented
        }
        presenter.present(activityViewController, animated: true)
    }
}
