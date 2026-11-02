import Foundation

/// Descriptive metadata shared by every book format Kind understands.
nonisolated struct BookMetadata: Codable, Hashable, Sendable {
    var title: String = ""
    var authors: [String] = []
    var publisher: String = ""
    var summary: String = ""
    var language: String = ""
    var publishedDate: String = ""
    var subjects: [String] = []
    var isbn: String = ""
    var asin: String = ""
    /// Kindle content type: `EBOK` (store book) or `PDOC` (personal document).
    var cdeType: String = ""
    var pageCount: Int?
    /// Uncompressed text length for MOBI-family books; used to turn reading positions into a percentage.
    var textLength: Int?
    var isDRMProtected = false

    var authorLine: String { authors.joined(separator: ", ") }
}

/// A user's edits from the metadata editor. `nil` fields are left untouched.
nonisolated struct MetadataChanges: Sendable, Equatable {
    var title: String?
    var authors: [String]?
    var publisher: String?
    var summary: String?
    var language: String?
    var publishedDate: String?
    var subjects: [String]?
    /// New cover image (any format `NSImage` can read); re-encoded as JPEG before writing.
    var coverImage: Data?

    var isEmpty: Bool {
        title == nil && authors == nil && publisher == nil && summary == nil &&
            language == nil && publishedDate == nil && subjects == nil && coverImage == nil
    }
}

nonisolated enum BookFormat: String, Codable, CaseIterable, Sendable {
    case pdf, mobi, azw, azw3, kfx, txt, docx, epub

    init?(pathExtension: String) {
        switch pathExtension.lowercased() {
        case "pdf": self = .pdf
        case "mobi", "prc": self = .mobi
        case "azw": self = .azw
        case "azw3", "azw8": self = .azw3
        case "kfx", "azw9", "kfx-zip": self = .kfx
        case "txt": self = .txt
        case "docx", "doc": self = .docx
        case "epub": self = .epub
        default: return nil
        }
    }

    var label: String { rawValue.uppercased() }

    /// MOBI container formats that Kind can parse and rewrite.
    var isMOBIFamily: Bool { self == .mobi || self == .azw || self == .azw3 }
    var supportsMetadataEditing: Bool { isMOBIFamily || self == .pdf }

    /// Every file extension Kind recognises as a book. All of them can be sent to the Kindle
    /// as-is; no conversion happens.
    static let fileExtensions = ["pdf", "epub", "mobi", "prc", "azw", "azw3", "azw8", "kfx", "azw9", "kfx-zip",
                                 "txt", "doc", "docx"]

    static func isUploadable(_ url: URL) -> Bool {
        BookFormat(pathExtension: url.pathExtension) != nil
    }
}
