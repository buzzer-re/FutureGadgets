import Foundation

nonisolated struct Book: Identifiable, Hashable, Sendable, Codable {
    var path: String
    var format: BookFormat
    var size: Int
    var modified: Date?
    var metadata: BookMetadata
    var progress: ReadingProgress?
    /// Path of the `.sdr` sidecar folder, when the Kindle has created one.
    var sidecarPath: String?
    /// Path of a Kindle-generated thumbnail in `system/thumbnails`, used for store books.
    var thumbnailPath: String?
    var hasEmbeddedCover = false
    /// Whether the full metadata pass has run (filename-only entries show up first).
    var isEnriched = false

    var id: String { path }
    var fileName: String { (path as NSString).lastPathComponent }
    var folder: String { (path as NSString).deletingLastPathComponent }

    var title: String {
        metadata.title.isEmpty ? Self.cleanTitle(fromFileName: fileName) : metadata.title
    }

    var authorLine: String { metadata.authors.isEmpty ? (fileNameAuthor ?? "") : metadata.authorLine }

    var readingState: ReadingProgress.State { progress?.state ?? .unread }

    /// Cache key that changes whenever the file on the device changes.
    var contentKey: String {
        "\(path)|\(size)|\(Int(modified?.timeIntervalSince1970 ?? 0))"
    }

    var asin: String? {
        if !metadata.asin.isEmpty { return metadata.asin }
        return Self.asin(inFileName: fileName)
    }

    static func asin(inFileName name: String) -> String? {
        guard let range = name.range(of: "B0[0-9A-Z]{8}", options: .regularExpression) else { return nil }
        return String(name[range])
    }

    /// `The_Hobbit-asin_B000FA5ZEG-type_EBOK-v_0.azw` → `The Hobbit`
    /// `The Lighthouse_ A Novel_B00TESTASN_sample.kfx` → `The Lighthouse: A Novel`
    static func cleanTitle(fromFileName name: String) -> String {
        var title = (name as NSString).deletingPathExtension
        for pattern in ["-asin_.*$", "_B0[0-9A-Z]{8}.*$", "_EBOK$", "_PDOC$"] {
            title = title.replacingOccurrences(of: pattern, with: "", options: .regularExpression)
        }
        // Amazon writes ":" as "_" in file names.
        title = title.replacingOccurrences(of: "_ (?!\\()", with: ": ", options: .regularExpression)
        title = title.replacingOccurrences(of: "_", with: " ")
        title = title.replacingOccurrences(of: "\\s+", with: " ", options: .regularExpression)
        if let split = splitAuthor(title) { title = split.title }
        title = title.trimmingCharacters(in: .whitespaces)
        return title.isEmpty ? name : title
    }

    /// `Title - Author` is a common naming convention for downloaded books.
    static func splitAuthor(_ baseName: String) -> (title: String, author: String?)? {
        guard let range = baseName.range(of: " - ", options: .backwards) else { return nil }
        let title = String(baseName[..<range.lowerBound]).trimmingCharacters(in: .whitespaces)
        let author = String(baseName[range.upperBound...]).trimmingCharacters(in: .whitespaces)
        guard !title.isEmpty, !author.isEmpty, author.count < 60 else { return nil }
        return (title, ["unknown", "anonymous"].contains(author.lowercased()) ? nil : author)
    }

    /// Author guessed from the file name, used when the book itself has none.
    var fileNameAuthor: String? {
        Self.splitAuthor((fileName as NSString).deletingPathExtension)?.author
    }
}

nonisolated enum LibraryFilter: String, CaseIterable, Identifiable, Hashable, Codable {
    case all, reading, unread, finished, pdfs, ebooks, documents

    var id: String { rawValue }

    var title: String {
        switch self {
        case .all: "All Books"
        case .reading: "Reading Now"
        case .unread: "Unread"
        case .finished: "Finished"
        case .pdfs: "PDFs"
        case .ebooks: "Kindle Books"
        case .documents: "Documents"
        }
    }

    var symbol: String {
        switch self {
        case .all: "books.vertical"
        case .reading: "book"
        case .unread: "sparkles"
        case .finished: "checkmark.seal"
        case .pdfs: "doc.richtext"
        case .ebooks: "text.book.closed"
        case .documents: "doc.text"
        }
    }

    func includes(_ book: Book) -> Bool {
        switch self {
        case .all: true
        case .reading: book.readingState == .reading
        case .unread: book.readingState == .unread
        case .finished: book.readingState == .finished
        case .pdfs: book.format == .pdf
        case .ebooks: book.format.isMOBIFamily || book.format == .kfx
        case .documents: [.txt, .docx, .epub].contains(book.format)
        }
    }
}

nonisolated enum LibrarySort: String, CaseIterable, Identifiable, Codable {
    case recent, title, author, progress, size

    var id: String { rawValue }

    var title: String {
        switch self {
        case .recent: "Recently Read"
        case .title: "Title"
        case .author: "Author"
        case .progress: "Progress"
        case .size: "File Size"
        }
    }

    func sorted(_ books: [Book]) -> [Book] {
        switch self {
        case .recent:
            books.sorted {
                let a = $0.progress?.lastRead ?? $0.modified ?? .distantPast
                let b = $1.progress?.lastRead ?? $1.modified ?? .distantPast
                return a > b
            }
        case .title:
            books.sorted { $0.title.localizedStandardCompare($1.title) == .orderedAscending }
        case .author:
            books.sorted { ($0.authorLine, $0.title).0.localizedStandardCompare($1.authorLine) == .orderedAscending }
        case .progress:
            books.sorted { ($0.progress?.fraction ?? 0) > ($1.progress?.fraction ?? 0) }
        case .size:
            books.sorted { $0.size > $1.size }
        }
    }
}
