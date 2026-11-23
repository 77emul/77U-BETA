import SwiftUI

struct DiagnosticsActionsView<Open: View, Copy: View, Save: View>: View {
    @ViewBuilder let open: () -> Open
    @ViewBuilder let copy: () -> Copy
    @ViewBuilder let save: () -> Save
    let close: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Developer diagnostics")
                .font(.title2.bold())
                .fixedSize(horizontal: false, vertical: true)
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 140), spacing: 8)], spacing: 8) {
                open()
                copy()
                save()
                Button("Close", systemImage: "xmark", action: close)
                    .controllerButtonStyle(.bordered)
                    .frame(maxWidth: .infinity, minHeight: 44)
            }
        }
        .padding(.bottom, 12)
    }
}
