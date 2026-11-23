import SwiftUI

struct LibraryLaunchScreen: View {
    let game: GameLibraryEntry
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var appeared = false

    var body: some View {
        GeometryReader { proxy in
            let side = min(260, min(proxy.size.width, proxy.size.height) * 0.38)
            ZStack {
                Color.black
                VStack(spacing: 28) {
                    if let artwork = game.icon {
                        Image(uiImage: artwork)
                            .resizable()
                            .interpolation(.high)
                            .scaledToFit()
                            .frame(width: side, height: side)
                            .clipShape(RoundedRectangle(cornerRadius: side * 0.12, style: .continuous))
                            .scaleEffect(appeared || reduceMotion ? 1 : 0.96)
                            .accessibilityHidden(true)
                    }
                    VStack(spacing: 18) {
                        Text(game.title)
                            .font(.system(size: 17, weight: .medium, design: .rounded))
                            .foregroundStyle(.white.opacity(0.8))
                            .multilineTextAlignment(.center)
                            .lineLimit(2)
                        ProgressView()
                            .tint(.white.opacity(0.55))
                            .accessibilityLabel("Starting game")
                    }
                    .frame(maxWidth: 380)
                }
                .padding(32)
                .opacity(appeared ? 1 : 0)
            }
            .frame(width: proxy.size.width, height: proxy.size.height)
        }
        .ignoresSafeArea()
        .accessibilityIdentifier("library.launchScreen")
        .onAppear {
            withAnimation(.easeOut(duration: reduceMotion ? 0.12 : 0.36)) { appeared = true }
        }
    }
}
