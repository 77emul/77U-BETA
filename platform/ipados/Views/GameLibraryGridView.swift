import SwiftUI
import UIKit

struct GameLibraryGridView: View {
    @ObservedObject private var theme = MenuThemeStore.shared
    let games: [GameLibraryEntry]
    let play: (GameLibraryEntry) async -> Bool
    let manageSaves: (GameLibraryEntry) -> Void
    let manageGraphics: (GameLibraryEntry) -> Void
    var isEnhancingArtwork = false
    @AppStorage("CemuIOS.SelectedLibraryGame") private var savedSelectedID = ""
    @State private var selectedID = ""
    @State private var selectionInitialized = false
    @State private var page = 0
    @State private var launchingID: String?
    @Environment(\.colorScheme) private var colorScheme
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private var selectedGame: GameLibraryEntry? {
        games.first(where: { $0.id == selectedID }) ?? games.first
    }

    var body: some View {
        GeometryReader { proxy in
            let phone = UIDevice.current.userInterfaceIdiom == .phone
            let short = phone && proxy.size.width > proxy.size.height
            let columns = proxy.size.width < 600 ? 3 : (proxy.size.width < 900 ? 4 : 5)
            let rows = short ? 1 : (proxy.size.height > 820 ? 3 : 2)
            let capacity = columns * rows
            let pageCount = max(1, (games.count + capacity - 1) / capacity)

            VStack(spacing: 12) {
                if short {
                    HStack(spacing: 12) {
                        pages(columns: columns, rows: rows, capacity: capacity, pageCount: pageCount)
                        dock(compact: true).frame(width: 320)
                    }
                } else {
                    pages(columns: columns, rows: rows, capacity: capacity, pageCount: pageCount)
                    pageControls(pageCount: pageCount)
                    dock(compact: proxy.size.width < 760)
                        .padding(.horizontal, phone ? 0 : 32)
                }
            }
            .padding(.bottom, phone ? 12 : max(24, proxy.safeAreaInsets.bottom + 10))
            .onAppear { restoreSelection(capacity: capacity) }
            .onChange(of: games.map(\.id)) { restoreSelection(capacity: capacity) }
            .onChange(of: capacity) { restoreSelection(capacity: capacity) }
            .onChange(of: page) { _, newPage in
                // Keep the Start action attached to a tile on the visible page.
                if let index = games.firstIndex(where: { $0.id == selectedID }), index / capacity != newPage {
                    let first = newPage * capacity
                    if games.indices.contains(first) { selectedID = games[first].id }
                }
            }
        }
        .onChange(of: selectedID) { _, id in
            if savedSelectedID != id { savedSelectedID = id }
        }
        .allowsHitTesting(launchingID == nil)
        .task(id: launchingID) {
            guard let id = launchingID, let game = games.first(where: { $0.id == id }) else { return }
            do {
                if !reduceMotion { try await Task.sleep(for: .milliseconds(320)) }
                try Task.checkCancellation()
                if !(await play(game)), !Task.isCancelled {
                    withAnimation(reduceMotion ? nil : .easeInOut(duration: 0.24)) { launchingID = nil }
                }
            } catch { launchingID = nil }
        }
    }

    private func pages(columns: Int, rows: Int, capacity: Int, pageCount: Int) -> some View {
        GeometryReader { proxy in
            let spacing: CGFloat = proxy.size.width < 600 ? 14 : 24
            let side = max(42, min(152,
                (proxy.size.width - 48 - CGFloat(columns - 1) * spacing) / CGFloat(columns),
                (proxy.size.height - 24 - CGFloat(rows - 1) * 14) / CGFloat(rows) - 26))
            TabView(selection: $page) {
                ForEach(0..<pageCount, id: \.self) { pageIndex in
                    let first = pageIndex * capacity
                    let entries = Array(games.dropFirst(first).prefix(capacity))
                    LazyVGrid(columns: Array(repeating: GridItem(.fixed(side), spacing: spacing), count: columns), spacing: 14) {
                        ForEach(entries) { game in
                            tile(game, side: side)
                        }
                        ForEach(entries.count..<capacity, id: \.self) { _ in
                            RoundedRectangle(cornerRadius: side * 0.20, style: .continuous)
                                .fill(Color.primary.opacity(0.018))
                                .overlay(RoundedRectangle(cornerRadius: side * 0.20, style: .continuous)
                                    .strokeBorder(Color.primary.opacity(0.065), lineWidth: 0.8))
                                .frame(width: side, height: side)
                                .padding(.bottom, 26)
                                .accessibilityHidden(true)
                        }
                    }
                    .padding(.vertical, 12)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .tag(pageIndex)
                }
            }
            .tabViewStyle(.page(indexDisplayMode: .never))
        }
    }

    private func tile(_ game: GameLibraryEntry, side: CGFloat) -> some View {
        let selected = game.id == selectedID
        return Button {
            if selected { beginLaunch(game); return }
            withAnimation(reduceMotion ? nil : .spring(response: 0.32, dampingFraction: 0.78)) {
                selectedID = game.id
            }
            UISelectionFeedbackGenerator().selectionChanged()
        } label: {
            VStack(spacing: 8) {
                Group {
                    if let icon = game.icon {
                        Image(uiImage: icon).resizable().interpolation(.high).scaledToFit()
                    } else {
                        Image(systemName: "gamecontroller")
                            .font(.system(size: side * 0.28, weight: .light))
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                            .foregroundStyle(.secondary)
                    }
                }
                .frame(width: side - 12, height: side - 12)
                .clipShape(RoundedRectangle(cornerRadius: max(8, side * 0.20 - 6), style: .continuous))
                .padding(6)
                .background(colorScheme == .light ? Color.white : Color(white: 0.10),
                            in: RoundedRectangle(cornerRadius: side * 0.20, style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: side * 0.20, style: .continuous)
                    .strokeBorder(Color.primary.opacity(selected ? 0.25 : 0.10),
                                  lineWidth: selected ? 1 : 0.8))
                .shadow(color: .black.opacity(colorScheme == .light ? 0.08 : 0.24),
                        radius: selected ? 12 : 5, y: selected ? 6 : 3)
                .scaleEffect(isEnhancingArtwork && !reduceMotion ? 0.92 : 1)
                .animation(reduceMotion ? nil : .easeInOut(duration: 0.35), value: isEnhancingArtwork)
                .scaleEffect(selected && !reduceMotion ? 1.055 : 1)
                .offset(y: selected && !reduceMotion ? -3 : 0)
                .animation(reduceMotion ? nil : .spring(response: 0.34, dampingFraction: 0.84), value: selected)
                .scaleEffect(launchingID == game.id && !reduceMotion ? 1.035 : 1)
                .opacity(launchingID == game.id && !reduceMotion ? 0.82 : 1)

                Text(game.title)
                    .font(.system(size: 11, weight: selected ? .semibold : .medium, design: .rounded))
                    .foregroundStyle(selected ? LibraryTheme.selection(for: colorScheme) : Color.secondary)
                    .lineLimit(1)
                    .frame(height: 18)
            }
            .contentShape(Rectangle())
        }
        .controllerButtonStyle(LibraryTilePressStyle())
        .zIndex(selected ? 1 : 0)
        .accessibilityLabel(game.title)
        .accessibilityAddTraits(selected ? .isSelected : [])
        .contextMenu {
            Button(theme.configuration.enabled ? theme.configuration.playTitle : "Play", systemImage: theme.configuration.enabled ? theme.configuration.playIcon : "play.fill") { beginLaunch(game) }
            Button("Saves", systemImage: "externaldrive") { manageSaves(game) }
                .disabled(game.numericTitleID == nil)
            Button("Graphics & mods", systemImage: "slider.horizontal.3") { manageGraphics(game) }
                .disabled(game.numericTitleID == nil)
        }
    }

    private func pageControls(pageCount: Int) -> some View {
        HStack(spacing: 16) {
            Button { changePage(by: -1, pageCount: pageCount) } label: {
                Image(systemName: "chevron.left").frame(width: 44, height: 44)
            }
            .disabled(page == 0)
            .opacity(page == 0 ? 0.25 : 1)
            .accessibilityLabel("Previous page")
            .accessibilityIdentifier("library.previousPage")
            if pageCount <= 10 {
                HStack(spacing: 7) {
                    ForEach(0..<pageCount, id: \.self) { index in
                        RoundedRectangle(cornerRadius: 3)
                            .fill(LibraryTheme.selection(for: colorScheme).opacity(index == page ? 1 : 0.18))
                            .frame(width: index == page ? 18 : 7, height: 7)
                    }
                }
                .accessibilityLabel("Page \(page + 1) of \(pageCount)")
            } else {
                Text("\(page + 1) / \(pageCount)").font(.caption.monospacedDigit())
            }
            Button { changePage(by: 1, pageCount: pageCount) } label: {
                Image(systemName: "chevron.right").frame(width: 44, height: 44)
            }
            .disabled(page >= pageCount - 1)
            .opacity(page >= pageCount - 1 ? 0.25 : 1)
            .accessibilityLabel("Next page")
            .accessibilityIdentifier("library.nextPage")
        }
        .font(.system(size: 12, weight: .bold))
        .foregroundStyle(LibraryTheme.selection(for: colorScheme))
        .controllerButtonStyle(.plain)
    }

    @ViewBuilder private func dock(compact: Bool) -> some View {
        if let game = selectedGame {
            LibraryGameDock(game: game, compact: compact,
                            play: beginLaunch, manageSaves: manageSaves, manageGraphics: manageGraphics)
        }
    }

    private func beginLaunch(_ game: GameLibraryEntry) {
        guard launchingID == nil else { return }
        withAnimation(reduceMotion ? nil : .easeInOut(duration: 0.32)) { launchingID = game.id }
    }

    private func changePage(by delta: Int, pageCount: Int) {
        withAnimation(reduceMotion ? nil : .easeInOut(duration: 0.25)) {
            page = min(pageCount - 1, max(0, page + delta))
        }
    }

    private func restoreSelection(capacity: Int) {
        let restored = GameCoverFlowGeometry.initialSelectionID(
            savedID: selectionInitialized ? selectedID : savedSelectedID, gameIDs: games.map(\.id))
        selectionInitialized = true
        if selectedID != restored { selectedID = restored }
        page = (games.firstIndex(where: { $0.id == restored }) ?? 0) / capacity
    }
}
