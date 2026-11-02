import SwiftUI
import UniformTypeIdentifiers

struct LibraryView: View {
    @Environment(AppModel.self) private var model
    let device: KindleDevice

    @State private var isDropTargeted = false

    private let columns = [GridItem(.adaptive(minimum: 128, maximum: 168), spacing: 24, alignment: .top)]

    var body: some View {
        @Bindable var model = model
        let books = model.visibleBooks

        Group {
            if device.phase == .listing || (device.phase == .connecting && device.books.isEmpty) {
                SkeletonGrid(columns: columns)
            } else if case .failed = device.phase, device.fileSystem == nil {
                emptyState
            } else if books.isEmpty {
                emptyState
            } else if model.viewMode == .grid {
                grid(books)
            } else {
                BookTable(device: device, books: books)
            }
        }
        .animation(.smooth(duration: 0.3), value: model.viewMode)
        .navigationTitle(model.filter == .all ? device.name : model.filter.title)
        .navigationSubtitle(subtitle(books.count))
        .searchable(text: $model.searchText, placement: .toolbar, prompt: "Title, author or file name")
        .toolbar { toolbar }
        .dropDestination(for: URL.self) { urls, _ in
            let accepted = urls.filter(BookFormat.isUploadable)
            model.actions?.upload(accepted)
            return !accepted.isEmpty
        } isTargeted: { targeted in
            withAnimation(.spring(duration: 0.3)) { isDropTargeted = targeted }
        }
        .overlay { if isDropTargeted { DropOverlay(deviceName: device.name) } }
        .onDeleteCommand { model.requestDelete(model.selectedBooks) }
        .fileImporter(isPresented: $model.isExporting, allowedContentTypes: [.folder]) { result in
            if case let .success(folder) = result {
                model.actions?.export(model.booksToExport, to: folder)
            }
        }
        .fileDialogConfirmationLabel("Save Here")
    }

    private func subtitle(_ count: Int) -> String {
        var parts = ["\(count) \(count == 1 ? "book" : "books")"]
        if case let .enriching(done, total) = device.phase, done < total { parts.append("indexing \(done)/\(total)") }
        return parts.joined(separator: " · ")
    }

    // MARK: Grid

    private func grid(_ books: [Book]) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 28) {
                let shelf = model.continueReading
                if model.filter == .all, model.searchText.isEmpty, !shelf.isEmpty {
                    VStack(alignment: .leading, spacing: 12) {
                        SectionHeader(title: "Continue Reading", symbol: "bookmark.fill")
                        ScrollView(.horizontal) {
                            LazyHStack(spacing: 16) {
                                ForEach(shelf) { book in
                                    ShelfCard(book: book, coverKey: device.coverKey(for: book))
                                        .onTapGesture { select(book, extend: false) }
                                        .onTapGesture(count: 2) { model.editingBook = book }
                                        .contextMenu { BookContextMenu(books: [book]) }
                                }
                            }
                            .padding(.vertical, 10)
                            .scrollTargetLayout()
                        }
                        .scrollIndicators(.never)
                        .scrollTargetBehavior(.viewAligned)
                        .scrollClipDisabled()
                    }
                    SectionHeader(title: "Library", symbol: "books.vertical.fill")
                }

                LazyVGrid(columns: columns, alignment: .leading, spacing: 30) {
                    ForEach(books) { book in
                        BookCard(book: book, coverKey: device.coverKey(for: book), isSelected: model.selection.contains(book.id))
                            .onTapGesture(count: 2) { model.editingBook = book }
                            .onTapGesture { select(book, extend: NSEvent.modifierFlags.contains(.command)) }
                            .contextMenu { BookContextMenu(books: contextBooks(for: book)) }
                            .transition(.scale(scale: 0.9).combined(with: .opacity))
                    }
                }
                .animation(.spring(duration: 0.45, bounce: 0.2), value: books.map(\.id))
            }
            .padding(.horizontal, 28)
            .padding(.vertical, 20)
            .padding(.bottom, 60)
        }
        .contentShape(.rect)
        .onTapGesture { model.selection.removeAll() }
    }

    private func select(_ book: Book, extend: Bool) {
        withAnimation(.snappy(duration: 0.2)) {
            if extend {
                if model.selection.contains(book.id) { model.selection.remove(book.id) } else { model.selection.insert(book.id) }
            } else {
                model.selection = [book.id]
            }
        }
    }

    private func contextBooks(for book: Book) -> [Book] {
        model.selection.contains(book.id) ? model.selectedBooks : [book]
    }

    // MARK: Empty state

    @ViewBuilder
    private var emptyState: some View {
        if !model.searchText.isEmpty {
            ContentUnavailableView.search(text: model.searchText)
        } else if case let .failed(message) = device.phase {
            ContentUnavailableView {
                Label("Couldn't read the Kindle", systemImage: "exclamationmark.triangle")
            } description: {
                Text(message)
            } actions: {
                Button("Try Again") { model.devices.reconnect(device) }
            }
        } else {
            ContentUnavailableView {
                Label(model.filter == .all ? "No books yet" : "Nothing in \(model.filter.title)", systemImage: model.filter.symbol)
            } description: {
                Text("Drag books here (PDF, EPUB, MOBI, AZW3 and more) to send them to \(device.name).")
            } actions: {
                Button("Add Books…") { model.isImporting = true }
                    .buttonStyle(.borderedProminent)
            }
        }
    }

    // MARK: Toolbar

    @ToolbarContentBuilder
    private var toolbar: some ToolbarContent {
        @Bindable var model = model
        ToolbarItemGroup(placement: .primaryAction) {
            Button { model.isImporting = true } label: {
                Label("Add Books", systemImage: "plus")
            }
            .help("Send PDFs and books to the Kindle (⌘O)")

            Picker("View", selection: $model.viewMode) {
                ForEach(AppModel.ViewMode.allCases) { mode in
                    Label(mode.title, systemImage: mode.symbol).tag(mode)
                }
            }
            .pickerStyle(.segmented)
            .help("View as covers or list")

            Menu {
                Picker("Sort By", selection: $model.sort) {
                    ForEach(LibrarySort.allCases) { Text($0.title).tag($0) }
                }
                .pickerStyle(.inline)
            } label: {
                Label("Sort", systemImage: "arrow.up.arrow.down")
            }
            .help("Sort books")

            Button { device.refresh() } label: {
                Label("Refresh", systemImage: "arrow.clockwise")
                    .symbolEffect(.rotate, isActive: device.phase.isBusy)
            }
            .help("Re-read the library from the Kindle (⌘R)")

            Button { withAnimation { model.showInspector.toggle() } } label: {
                Label("Inspector", systemImage: "sidebar.trailing")
            }
            .help("Show or hide book details")
        }
    }
}

private struct SectionHeader: View {
    let title: String
    let symbol: String

    var body: some View {
        Label(title, systemImage: symbol)
            .font(.system(.title3, design: .serif, weight: .semibold))
            .foregroundStyle(.primary)
            .labelStyle(.titleOnly)
    }
}

struct BookContextMenu: View {
    @Environment(AppModel.self) private var model
    let books: [Book]

    var body: some View {
        if books.count == 1, let book = books.first {
            Button("Edit Info…", systemImage: "pencil") { model.editingBook = book }
                .disabled(!book.format.supportsMetadataEditing || book.metadata.isDRMProtected)
        }
        Button(books.count == 1 ? "Save to Mac…" : "Save \(books.count) Books to Mac…", systemImage: "square.and.arrow.down") {
            model.booksToExport = books
            model.isExporting = true
        }
        if let url = model.selectedDevice?.volumeURL, books.count == 1, let book = books.first {
            Button("Show in Finder", systemImage: "folder") {
                NSWorkspace.shared.activateFileViewerSelecting([url.appendingPathComponent(book.path)])
            }
        }
        Divider()
        Button(books.count == 1 ? "Delete from Kindle…" : "Delete \(books.count) Books from Kindle…",
               systemImage: "trash", role: .destructive) {
            model.requestDelete(books)
        }
    }
}

private struct DropOverlay: View {
    let deviceName: String
    @State private var bounce = false

    var body: some View {
        ZStack {
            Rectangle().fill(.ultraThinMaterial)
            RoundedRectangle(cornerRadius: 24)
                .strokeBorder(Color.accentColor, style: StrokeStyle(lineWidth: 3, dash: [12, 8]))
                .padding(24)
            VStack(spacing: 14) {
                Image(systemName: "arrow.down.doc.fill")
                    .font(.system(size: 54))
                    .foregroundStyle(Color.accentColor)
                    .symbolEffect(.bounce, value: bounce)
                Text("Drop to send to \(deviceName)")
                    .font(.system(.title2, design: .serif, weight: .semibold))
                Text("PDF, EPUB, MOBI, AZW, AZW3, KFX, TXT, DOC and DOCX")
                    .foregroundStyle(.secondary)
            }
        }
        .transition(.opacity)
        .onAppear { bounce.toggle() }
        .allowsHitTesting(false)
    }
}

private struct SkeletonGrid: View {
    let columns: [GridItem]
    @State private var shimmer = false

    var body: some View {
        ScrollView {
            LazyVGrid(columns: columns, spacing: 30) {
                ForEach(0 ..< 18, id: \.self) { index in
                    VStack(alignment: .leading, spacing: 8) {
                        RoundedRectangle(cornerRadius: Theme.coverCorner)
                            .fill(.quaternary)
                            .aspectRatio(Theme.coverAspect, contentMode: .fit)
                        RoundedRectangle(cornerRadius: 3).fill(.quaternary).frame(height: 10)
                        RoundedRectangle(cornerRadius: 3).fill(.quaternary).frame(width: 70, height: 8)
                    }
                    .opacity(shimmer ? 0.45 : 1)
                    .animation(.easeInOut(duration: 0.9).repeatForever().delay(Double(index % 6) * 0.08), value: shimmer)
                }
            }
            .padding(28)
        }
        .onAppear { shimmer = true }
        .allowsHitTesting(false)
    }
}
