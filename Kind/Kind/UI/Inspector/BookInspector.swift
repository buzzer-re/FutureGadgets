import SwiftUI

struct InspectorContainer: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        Group {
            if let device = model.selectedDevice, let book = model.focusedBook {
                BookInspector(book: book, device: device)
                    .id(book.id)
                    .transition(.asymmetric(insertion: .opacity.combined(with: .offset(y: 8)), removal: .opacity))
            } else if model.selectedBooks.count > 1 {
                MultiSelectionSummary(books: model.selectedBooks)
            } else if let device = model.selectedDevice {
                DeviceSummary(device: device)
            } else {
                ContentUnavailableView("No Kindle", systemImage: "cable.connector", description: Text("Connect a Kindle to see its books."))
            }
        }
        .animation(.smooth(duration: 0.25), value: model.focusedBook?.id)
    }
}

struct BookInspector: View {
    @Environment(AppModel.self) private var model
    let book: Book
    let device: KindleDevice

    var body: some View {
        ScrollView {
            VStack(spacing: 22) {
                CoverView(book: book, coverKey: device.coverKey(for: book), showsProgress: false)
                    .frame(width: 180)
                    .shadow(color: .black.opacity(0.3), radius: 18, y: 12)
                    .padding(.top, 12)

                VStack(spacing: 6) {
                    Text(book.title)
                        .font(.system(.title2, design: .serif, weight: .semibold))
                        .multilineTextAlignment(.center)
                        .textSelection(.enabled)
                    if !book.authorLine.isEmpty {
                        Text(book.authorLine).font(.headline).foregroundStyle(.secondary)
                    }
                }

                progressCard

                HStack(spacing: 10) {
                    Button { model.editingBook = book } label: {
                        Label("Edit Info", systemImage: "pencil")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(!book.format.supportsMetadataEditing || book.metadata.isDRMProtected)
                    .help(editHelp)

                    Button {
                        model.booksToExport = [book]
                        model.isExporting = true
                    } label: {
                        Image(systemName: "square.and.arrow.down")
                    }
                    .help("Save a copy to your Mac")

                    Button(role: .destructive) { model.requestDelete([book]) } label: {
                        Image(systemName: "trash")
                    }
                    .help("Delete from Kindle")
                }
                .controlSize(.large)

                details

                if !book.metadata.summary.isEmpty {
                    VStack(alignment: .leading, spacing: 6) {
                        Text("Description").font(.headline)
                        Text(book.metadata.summary)
                            .font(.callout)
                            .foregroundStyle(.secondary)
                            .textSelection(.enabled)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .padding(20)
        }
    }

    private var editHelp: String {
        if book.metadata.isDRMProtected { return "DRM-protected books can't be edited." }
        if !book.format.supportsMetadataEditing { return "\(book.format.label) files can't be edited." }
        return "Edit title, author, cover and description (⌘I)"
    }

    private var progressCard: some View {
        HStack(spacing: 16) {
            ProgressRing(fraction: book.progress?.fraction, tint: book.readingState.tint)
                .frame(width: 70, height: 70)
            VStack(alignment: .leading, spacing: 4) {
                Text(book.readingState.label)
                    .font(.headline)
                    .foregroundStyle(book.readingState.tint)
                if let date = book.progress?.lastRead {
                    Text("Last read \(date, format: .relative(presentation: .named))")
                        .font(.caption).foregroundStyle(.secondary)
                } else if book.progress == nil {
                    Text("Not opened on this Kindle yet").font(.caption).foregroundStyle(.secondary)
                }
                if let progress = book.progress, progress.fraction == nil, progress.position != nil {
                    Text("Exact position isn't available for this format")
                        .font(.caption2).foregroundStyle(.tertiary)
                }
                if let annotations = book.progress?.annotationCount, annotations > 0 {
                    Label("\(annotations) highlights & notes", systemImage: "highlighter")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(14)
        .background(.quaternary.opacity(0.45), in: .rect(cornerRadius: Theme.cardCorner))
    }

    private var details: some View {
        Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 12, verticalSpacing: 8) {
            DetailRow("Format", book.format.label + (book.metadata.isDRMProtected ? " · DRM" : ""))
            DetailRow("Size", book.size.byteString)
            if let pages = book.metadata.pageCount { DetailRow("Pages", pages.formatted()) }
            if !book.metadata.publisher.isEmpty { DetailRow("Publisher", book.metadata.publisher) }
            if !book.metadata.publishedDate.isEmpty { DetailRow("Published", String(book.metadata.publishedDate.prefix(10))) }
            if !book.metadata.language.isEmpty { DetailRow("Language", Locale.current.localizedString(forIdentifier: book.metadata.language) ?? book.metadata.language) }
            if let asin = book.asin { DetailRow("ASIN", asin) }
            if !book.metadata.subjects.isEmpty { DetailRow("Subjects", book.metadata.subjects.joined(separator: ", ")) }
            if let modified = book.modified { DetailRow("Added", modified.formatted(date: .abbreviated, time: .omitted)) }
            DetailRow("Location", book.path)
        }
        .font(.callout)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

private struct DetailRow: View {
    let label: String
    let value: String

    init(_ label: String, _ value: String) {
        self.label = label
        self.value = value
    }

    var body: some View {
        GridRow {
            Text(label).foregroundStyle(.secondary).gridColumnAlignment(.trailing)
            Text(value).textSelection(.enabled).lineLimit(3).truncationMode(.middle)
        }
    }
}

private struct MultiSelectionSummary: View {
    @Environment(AppModel.self) private var model
    let books: [Book]

    var body: some View {
        VStack(spacing: 20) {
            if let device = model.selectedDevice {
                ZStack {
                    ForEach(Array(books.prefix(4).enumerated()), id: \.element.id) { index, book in
                        CoverView(book: book, coverKey: device.coverKey(for: book), showsProgress: false)
                            .frame(width: 120)
                            .shadow(color: .black.opacity(0.25), radius: 8, y: 5)
                            .rotationEffect(.degrees(Double(index - min(books.count, 4) / 2) * 7))
                            .offset(x: CGFloat(index) * 14)
                    }
                }
                .frame(height: 220)
            }
            Text("\(books.count) Books Selected").font(.system(.title2, design: .serif, weight: .semibold))
            Text(books.reduce(0) { $0 + $1.size }.byteString).foregroundStyle(.secondary)
            VStack(spacing: 8) {
                Button {
                    model.booksToExport = books
                    model.isExporting = true
                } label: {
                    Label("Save to Mac…", systemImage: "square.and.arrow.down").frame(maxWidth: .infinity)
                }
                Button(role: .destructive) { model.requestDelete(books) } label: {
                    Label("Delete from Kindle…", systemImage: "trash").frame(maxWidth: .infinity)
                }
            }
            .controlSize(.large)
            .frame(maxWidth: 220)
        }
        .padding()
        .frame(maxHeight: .infinity)
    }
}

private struct DeviceSummary: View {
    @Environment(AppModel.self) private var model
    let device: KindleDevice

    var body: some View {
        let books = device.books
        let reading = books.filter { $0.readingState == .reading }.count
        let finished = books.filter { $0.readingState == .finished }.count
        let highlights = books.reduce(0) { $0 + ($1.progress?.annotationCount ?? 0) }

        ScrollView {
            VStack(spacing: 22) {
                KindleGlyph()
                    .frame(width: 90)
                    .shadow(color: .black.opacity(0.25), radius: 14, y: 8)
                    .padding(.top, 20)
                VStack(spacing: 4) {
                    Text(device.name).font(.system(.title2, design: .serif, weight: .semibold))
                    Text(device.model).foregroundStyle(.secondary)
                }
                if let storage = device.storage {
                    StorageBar(storage: storage).padding(.horizontal, 8)
                }
                Grid(horizontalSpacing: 12, verticalSpacing: 12) {
                    GridRow {
                        StatTile(value: books.count, label: "Books", symbol: "books.vertical")
                        StatTile(value: reading, label: "Reading", symbol: "book")
                    }
                    GridRow {
                        StatTile(value: finished, label: "Finished", symbol: "checkmark.seal")
                        StatTile(value: highlights, label: "Highlights", symbol: "highlighter")
                    }
                }
                if let synced = device.lastSynced {
                    Text("Synced \(synced, format: .relative(presentation: .named))")
                        .font(.caption).foregroundStyle(.tertiary)
                }
                Text("Select a book to see its details.")
                    .font(.callout).foregroundStyle(.secondary)
            }
            .padding(20)
        }
    }
}

private struct StatTile: View {
    let value: Int
    let label: String
    let symbol: String

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Image(systemName: symbol).foregroundStyle(Color.accentColor)
            Text(value, format: .number)
                .font(.system(.title, design: .rounded, weight: .semibold))
                .contentTransition(.numericText(value: Double(value)))
            Text(label).font(.caption).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(12)
        .background(.quaternary.opacity(0.45), in: .rect(cornerRadius: 12))
    }
}
