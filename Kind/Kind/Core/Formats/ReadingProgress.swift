import Foundation

/// Where the reader is in a book, recovered from the Kindle's sidecar files.
nonisolated struct ReadingProgress: Codable, Hashable, Sendable {
    /// 0…1 when known.
    var fraction: Double?
    var lastRead: Date?
    var position: String?
    var furthestPosition: String?
    var highlights: Int = 0
    var notes: Int = 0
    var bookmarks: Int = 0

    var isFinished: Bool { (fraction ?? 0) >= 0.98 }
    var isStarted: Bool { (fraction ?? 0) > 0.001 || position != nil }

    enum State: String, Codable, Sendable { case unread, reading, finished }

    var state: State {
        if isFinished { return .finished }
        return isStarted ? .reading : .unread
    }

    var annotationCount: Int { highlights + notes + bookmarks }
}

nonisolated enum ProgressExtractor {
    /// One reading record found in a sidecar file.
    private struct Record {
        var position: String?
        var furthest: String?
        var date: Date?
        var percent: Double?
    }

    /// Builds a progress value from every KRDS sidecar found for a book.
    ///
    /// A book's `.sdr` folder often holds several generations of sidecars (e.g. old
    /// `.azw3f`/`.mbs` files plus the `.yjf` the Kindle writes after converting a PDF), so the
    /// record with the newest last-read timestamp wins.
    ///
    /// - Parameters:
    ///   - sidecars: raw contents of files in the book's `.sdr` folder.
    ///   - metadata: used to convert absolute positions into a fraction
    ///     (MOBI text length, PDF page count).
    static func progress(from sidecars: [Data], metadata: BookMetadata?) -> ReadingProgress? {
        let documents = sidecars.filter(KRDS.isKRDS).compactMap { try? KRDS.decode($0) }
        guard !documents.isEmpty else { return nil }

        var progress = ReadingProgress()
        var records: [Record] = []
        for document in documents {
            if let lpr = document.field(named: "lpr") {
                let (position, date) = positionAndDate(in: lpr)
                records.append(Record(position: position,
                                      furthest: document.field(named: "fpr").flatMap { positionAndDate(in: $0).0 },
                                      date: date,
                                      percent: percentOfBook(in: document)))
            }
            progress.highlights = max(progress.highlights, document.countFields { $0.contains("highlight") && !$0.hasSuffix("cache") })
            progress.notes = max(progress.notes, document.countFields { $0.hasSuffix(".note") })
            progress.bookmarks = max(progress.bookmarks, document.countFields { $0.hasSuffix(".bookmark") })
        }

        guard let latest = records.max(by: { ($0.date ?? .distantPast) < ($1.date ?? .distantPast) }) else {
            progress.fraction = documents.lazy.compactMap(percentOfBook).first
            return progress.fraction != nil || progress.annotationCount > 0 ? progress : nil
        }
        progress.position = latest.position
        progress.furthestPosition = latest.furthest
        progress.lastRead = latest.date
        progress.fraction = latest.position.flatMap { position in metadata.flatMap { fraction(for: position, metadata: $0) } }
            ?? latest.percent
        return progress
    }

    /// `book.info.store` carries `num.words` and `percent.of.book` (0…100 or 0…1 depending on firmware).
    static func percentOfBook(in document: KRDS.Document) -> Double? {
        guard let field = document.field(named: "book.info.store") else { return nil }
        let doubles = field.children.compactMap { value -> Double? in
            if case let .double(v) = value { return v }
            if case let .float(v) = value { return Double(v) }
            return nil
        }
        guard let value = doubles.last, value > 0, value <= 100 else { return nil }
        return normalize(value)
    }

    private static func normalize(_ value: Double) -> Double {
        min(1, max(0, value > 1 ? value / 100 : value))
    }

    /// `lpr`/`fpr` hold a position (string or integer) and a millisecond timestamp.
    static func positionAndDate(in field: KRDS.Value) -> (String?, Date?) {
        var position: String?
        var date: Date?
        for child in field.children {
            switch child {
            case let .string(s?) where position == nil && !s.isEmpty:
                position = s
            case let .long(v) where v > 1_000_000_000_000:
                date = Date(timeIntervalSince1970: TimeInterval(v) / 1000)
            case let .long(v) where position == nil && v >= 0:
                position = String(v)
            case let .int(v) where position == nil && v > 1:
                position = String(v)
            default:
                continue
            }
        }
        return (position, date)
    }

    /// Converts a reading position into a fraction of the book:
    /// - KFX-style positions (`AWwJAAAAAAAA:19922946`), which the Kindle also uses for PDFs it has
    ///   converted internally: the number after the colon carries the page index in its upper 16 bits.
    /// - MOBI byte offsets (`"12345"` or `"12345:0"`), divided by the text length.
    /// - Plain PDF page numbers.
    static func fraction(for position: String, metadata: BookMetadata) -> Double? {
        let parts = position.split(separator: ":")
        if parts.count >= 2, let head = parts.first, Double(head) == nil, let value = Int(parts[1]) {
            guard let pages = metadata.pageCount, pages > 0 else { return nil }
            let page = value >> 16
            guard page >= 0, page < pages, value & 0xFFFF < 0x100 else { return nil }
            return normalize(Double(page + 1) / Double(pages))
        }
        let head = parts.first.map(String.init) ?? position
        guard let value = Double(head), value >= 0 else { return nil }
        if let pages = metadata.pageCount, pages > 0 {
            return value < Double(pages) ? normalize((value + 1) / Double(pages)) : nil
        }
        if let length = metadata.textLength, length > 0, value <= Double(length) {
            return normalize(value / Double(length))
        }
        return nil
    }
}
