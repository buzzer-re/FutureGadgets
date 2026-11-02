import SwiftUI
import UniformTypeIdentifiers

/// Sheet for editing a book's title, authors, cover and description.
struct MetadataEditor: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss

    let book: Book
    let device: KindleDevice

    @State private var title: String
    @State private var authors: String
    @State private var publisher: String
    @State private var language: String
    @State private var publishedDate: String
    @State private var subjects: String
    @State private var summary: String
    @State private var coverData: Data?
    @State private var insertPDFCoverPage = true
    @State private var isPickingCover = false
    @State private var isCoverTargeted = false

    init(book: Book, device: KindleDevice) {
        self.book = book
        self.device = device
        let meta = book.metadata
        _title = State(initialValue: book.title)
        _authors = State(initialValue: meta.authors.joined(separator: ", "))
        _publisher = State(initialValue: meta.publisher)
        _language = State(initialValue: meta.language)
        _publishedDate = State(initialValue: String(meta.publishedDate.prefix(10)))
        _subjects = State(initialValue: meta.subjects.joined(separator: ", "))
        _summary = State(initialValue: meta.summary)
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(alignment: .top, spacing: 12) {
                coverWell
                    .padding([.top, .leading], 24)
                form
            }

            Divider()

            HStack {
                Label(footnote, systemImage: "info.circle")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Save to Kindle") { save() }
                    .keyboardShortcut(.defaultAction)
                    .buttonStyle(.borderedProminent)
                    .disabled(changes.isEmpty || title.trimmingCharacters(in: .whitespaces).isEmpty)
            }
            .padding(16)
        }
        .frame(width: 740, height: 560)
        .fileImporter(isPresented: $isPickingCover, allowedContentTypes: [.image]) { result in
            if case let .success(url) = result { loadCover(from: url) }
        }
    }

    private var footnote: String {
        if book.format == .pdf {
            return "Title, author and subject are written into the PDF. The Kindle may take a moment to re-index."
        }
        return "Changes are written into the book file and the Kindle's cover thumbnail."
    }

    // MARK: Cover

    private var coverWell: some View {
        VStack(spacing: 12) {
            ZStack {
                if let coverData, let image = NSImage(data: coverData) {
                    Image(nsImage: image)
                        .resizable()
                        .aspectRatio(contentMode: .fill)
                        .frame(width: 190, height: 285)
                        .clipShape(.rect(cornerRadius: Theme.coverCorner))
                        .transition(.scale(scale: 0.95).combined(with: .opacity))
                } else {
                    CoverView(book: previewBook, coverKey: device.coverKey(for: book), showsProgress: false)
                        .frame(width: 190)
                }
                if isCoverTargeted {
                    RoundedRectangle(cornerRadius: Theme.coverCorner)
                        .fill(Color.accentColor.opacity(0.25))
                        .strokeBorder(Color.accentColor, lineWidth: 3)
                }
            }
            .frame(width: 190, height: 285)
            .shadow(color: .black.opacity(0.28), radius: 14, y: 8)
            .dropDestination(for: Data.self) { items, _ in
                guard let data = items.first, NSImage(data: data) != nil else { return false }
                withAnimation(.spring) { coverData = data }
                return true
            } isTargeted: { isCoverTargeted = $0 }
            .animation(.smooth, value: isCoverTargeted)

            HStack {
                Button("Choose…") { isPickingCover = true }
                Button("Paste") { pasteCover() }
                if coverData != nil {
                    Button("Revert") { withAnimation { coverData = nil } }
                }
            }
            .controlSize(.small)

            if book.format == .pdf, coverData != nil {
                Toggle("Insert as first page", isOn: $insertPDFCoverPage)
                    .toggleStyle(.checkbox)
                    .font(.caption)
                    .help("The Kindle uses the first page of a PDF as its cover.")
            }
            if book.format == .pdf, coverData != nil, !insertPDFCoverPage {
                Text("Without a cover page the Kindle keeps showing page one.")
                    .font(.caption2).foregroundStyle(.secondary).frame(width: 190)
            }
        }
    }

    private var previewBook: Book {
        var preview = book
        preview.metadata.title = title
        preview.metadata.authors = splitList(authors)
        return preview
    }

    private func loadCover(from url: URL) {
        let accessing = url.startAccessingSecurityScopedResource()
        defer { if accessing { url.stopAccessingSecurityScopedResource() } }
        if let data = try? Data(contentsOf: url), NSImage(data: data) != nil {
            withAnimation(.spring) { coverData = data }
        }
    }

    private func pasteCover() {
        guard let image = NSImage(pasteboard: .general), let tiff = image.tiffRepresentation else { return }
        withAnimation(.spring) { coverData = tiff }
    }

    // MARK: Form

    private var form: some View {
        Form {
            Section {
                TextField("Title", text: $title, prompt: Text("Book title"))
                    .font(.system(.title3, design: .serif))
                TextField("Authors", text: $authors, prompt: Text("Separate multiple authors with commas"))
            }
            Section {
                TextField("Publisher", text: $publisher)
                    .disabled(book.format == .pdf)
                HStack {
                    TextField("Published", text: $publishedDate, prompt: Text("YYYY-MM-DD"))
                        .disabled(book.format == .pdf)
                    TextField("Language", text: $language, prompt: Text("en"))
                        .frame(width: 150)
                        .disabled(book.format == .pdf)
                }
                TextField(book.format == .pdf ? "Keywords" : "Subjects", text: $subjects, prompt: Text("Comma separated"))
            }
            Section(book.format == .pdf ? "Subject" : "Description") {
                TextEditor(text: $summary)
                    .font(.body)
                    .frame(minHeight: 110)
                    .scrollContentBackground(.hidden)
            }
        }
        .formStyle(.grouped)
        .scrollContentBackground(.hidden)
        .frame(minHeight: 440)
    }

    private func splitList(_ text: String) -> [String] {
        text.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
    }

    /// Only fields the user actually touched are written back.
    private var changes: MetadataChanges {
        let meta = book.metadata
        var changes = MetadataChanges()
        let trimmedTitle = title.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmedTitle != book.title { changes.title = trimmedTitle }
        if splitList(authors) != meta.authors { changes.authors = splitList(authors) }
        if publisher != meta.publisher { changes.publisher = publisher }
        if language != meta.language { changes.language = language }
        if publishedDate != String(meta.publishedDate.prefix(10)) { changes.publishedDate = publishedDate }
        if splitList(subjects) != meta.subjects { changes.subjects = splitList(subjects) }
        if summary != meta.summary { changes.summary = summary }
        changes.coverImage = coverData
        return changes
    }

    private func save() {
        LibraryActions(device: device, transfers: model.transfers)
            .applyMetadata(changes, to: book, insertPDFCoverPage: insertPDFCoverPage)
        dismiss()
    }
}
