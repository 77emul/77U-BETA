import SwiftUI

struct LibraryLayoutTransition<Covers: View, Grid: View>: View {
    let style: String
    @ViewBuilder let covers: () -> Covers
    @ViewBuilder let grid: () -> Grid
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        ZStack {
            if style == "covers" {
                covers()
                    .transition(.opacity)
                    .zIndex(1)
            } else {
                grid()
                    .transition(.opacity)
                    .zIndex(0)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        // AppStorage delivers layout changes outside the picker's animation transaction.
        .animation(.easeInOut(duration: reduceMotion ? 0.12 : 0.28), value: style)
    }
}

struct LibraryTilePressStyle: ButtonStyle {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed && !reduceMotion ? 0.98 : 1)
            .brightness(configuration.isPressed ? -0.035 : 0)
            .animation(reduceMotion ? nil : .spring(response: 0.24, dampingFraction: 0.76),
                       value: configuration.isPressed)
    }
}
