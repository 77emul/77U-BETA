// SPDX-License-Identifier: MPL-2.0
import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct JITSettingsView: View {
    @ObservedObject private var jit = JITCoordinator.shared
    @State private var importError: String?
    @State private var importing = false
    var body: some View {
        Form {
            Section("Activation") {
                Picker("JIT method", selection: $jit.method) {
                    ForEach(jit.availableMethods) { method in
                        Text(jit.inLiveContainer && method == .manual
                             ? "LiveContainer JIT (automatic)" : method.title).tag(method)
                    }
                }.disabled(jit.busy || jit.ready)
                if jit.inLiveContainer {
                    Text("Choose StikDebug in LiveContainer Settings -> JIT. For 77U, enable Launch with JIT and select universal.js from its JIT folder. LiveContainer activates JIT at launch.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
                LabeledContent("Status", value: jit.ready ? "JIT ready" : jit.status)
                if let error = importError ?? jit.error { Text(error).foregroundStyle(.red).textSelection(.enabled) }
                Button { Task { await jit.ensureReady() } } label: {
                    HStack {
                        Text(jit.ready ? "JIT is ready" : "Enable JIT")
                        Spacer()
                        if jit.busy { ProgressView() }
                        else { Image(systemName: jit.ready ? "checkmark" : "bolt") }
                    }
                }.disabled(jit.busy || jit.ready)
            }
            if jit.method == .builtIn && !jit.inLiveContainer {
                Section("Setup") {
                    Text("Import a pairing file for this device. Enable Developer Mode and set up LocalDevVPN. Tap Play to activate JIT.")
                        .foregroundStyle(.secondary)
                    LabeledContent("Pairing file", value: jit.pairingName)
                    Button("Import pairing file", systemImage: "doc.badge.plus") { importing = true }
                        .disabled(jit.busy)
                    if jit.hasPairing {
                        Button("Remove pairing file", role: .destructive) { jit.removePairing() }.disabled(jit.busy)
                    }
                }
                Section("Pairing with iLoader") {
                    Text("Connect this device to your computer and open Manage Pairing File in iLoader. Choose Place -> 77U, or Export and transfer the file to Files. Placed files are detected automatically.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            } else {
                Section {
                    Text("77U uses the Universal protocol. Keep StikDebug's Universal script selected when connecting manually.")
                        .foregroundStyle(.secondary)
                }
            }
        }
        .navigationTitle("JIT")
        .onAppear { jit.applicationBecameActive() }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.data, .propertyList, .xml], allowsMultipleSelection: false) { result in
            switch result {
            case .success(let urls): if let url = urls.first { importError = nil; jit.importPairing(url) }
            case .failure(let error): if (error as NSError).code != NSUserCancelledError { importError = "Couldn't open pairing file: \(error.localizedDescription)" }
            }
        }
    }
}

struct JITLicensesView: View {
    private let files = [
        "77U-OpenSource", "Cemu-MPL-2.0", "StikJIT-MPL-2.0", "StikDebug-AGPL-3.0",
        "idevice-MIT", "Cubeb-ISC", "ZArchive-MIT", "ImGui-MIT",
        "Metal-cpp-Apache-2.0", "Xbyak-AArch64-Apache-2.0", "NotoCJK-OFL", "FontAwesome-OFL-1.1", "Lato-OFL-1.1", "RealESRGAN-LICENSE",
        "RealESRGAN-Model-Source", "Cemu-Graphic-Packs-CC0", "FPSPlusPlus-Source",
        "Rust-Dependencies", "Rust-Standard-Library", "ThirdParty-Notices"
    ]
    var body: some View {
        List(files, id: \.self) { file in
            NavigationLink(file) {
                LicenseDocumentView(file: file)
            }
        }.navigationTitle("Licenses & source")
    }
}

private struct LicenseDocumentView: View {
    let file: String
    @State private var text: String?

    var body: some View {
        Group {
            if let text {
                LargeLicenseTextView(text: text)
            } else {
                ProgressView("Loading…")
            }
        }
        .navigationTitle(file)
        .navigationBarTitleDisplayMode(.inline)
        .task {
            guard let url = Bundle.main.url(forResource: file, withExtension: "txt", subdirectory: "Licenses") else {
                text = "License file unavailable"
                return
            }
            text = await Task.detached(priority: .userInitiated) {
                (try? String(contentsOf: url, encoding: .utf8)) ?? "License file unavailable"
            }.value
        }
    }
}

private struct LargeLicenseTextView: UIViewRepresentable {
    let text: String

    func makeUIView(context: Context) -> UITextView {
        let view = UITextView()
        view.isEditable = false
        view.isSelectable = true
        view.alwaysBounceVertical = true
        view.layoutManager.allowsNonContiguousLayout = true
        view.backgroundColor = .clear
        view.textContainerInset = UIEdgeInsets(top: 18, left: 16, bottom: 24, right: 16)
        view.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
        return view
    }

    func updateUIView(_ view: UITextView, context: Context) {
        guard view.text != text else { return }
        view.text = text
    }
}
