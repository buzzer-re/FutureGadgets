import Foundation
import Testing
import UniformTypeIdentifiers
@testable import Kind

@Suite("MOBI / AZW3")
struct MOBITests {
    @Test func readsMetadataAndCover() async throws {
        let summary = try await MOBIBook.read(from: DataSource(data: Fixtures.mobi()))
        #expect(summary.metadata.title == "Original Title")
        #expect(summary.metadata.authors == ["Ann Author"])
        #expect(summary.metadata.summary == "A fine book.")
        #expect(summary.metadata.asin == "B00TESTASN")
        #expect(summary.metadata.textLength == 10_000)
        #expect(summary.cover == Fixtures.jpeg)
    }

    @Test func rewriteChangesMetadataAndKeepsRecordsIntact() async throws {
        let original = Fixtures.mobi()
        let newCover = Data([0xFF, 0xD8, 0xFF, 0xDB] + Array(repeating: 7, count: 300))
        var changes = MetadataChanges()
        changes.title = "Brand New Title — ünïcødé"
        changes.authors = ["First Writer", "Second Writer"]
        changes.summary = "Rewritten"
        changes.language = "pt"

        let rewritten = try MOBIBook.rewrite(original, applying: changes, cover: newCover, thumbnail: nil)
        let summary = try await MOBIBook.read(from: DataSource(data: rewritten))
        #expect(summary.metadata.title == "Brand New Title — ünïcødé")
        #expect(summary.metadata.authors == ["First Writer", "Second Writer"])
        #expect(summary.metadata.summary == "Rewritten")
        #expect(summary.metadata.language == "pt")
        #expect(summary.metadata.asin == "B00TESTASN")
        #expect(summary.cover == newCover)

        let before = try PalmDatabase(data: original)
        let after = try PalmDatabase(data: rewritten)
        #expect(after.records.count == before.records.count)
        #expect(after.records[1].data == before.records[1].data)
        #expect(after.records.map(\.uniqueID) == before.records.map(\.uniqueID))
    }

    @Test func palmDatabaseRoundTrips() throws {
        let data = Fixtures.mobi()
        #expect(try PalmDatabase(data: data).serialized() == data)
    }

    @Test func refusesToEditDRMBooks() {
        let data = Fixtures.mobi(encrypted: true)
        #expect(throws: MOBIBook.EditError.self) {
            try MOBIBook.rewrite(data, applying: MetadataChanges(title: "x"), cover: nil, thumbnail: nil)
        }
    }
}

@Suite("Reading progress")
struct ProgressTests {
    @Test func krdsRoundTrip() throws {
        let values: [KRDS.Value] = [
            .int(1), .long(3),
            .field(name: "lpr", values: [.byte(2), .string("1234"), .long(1_750_000_000_000)]),
            .field(name: "book.info.store", values: [.long(80_000), .double(42.5)]),
            .field(name: "annotation.cache.object", values: [
                .field(name: "annotation.personal.highlight", values: [.string("a")]),
                .field(name: "annotation.personal.highlight", values: [.string("b")]),
                .field(name: "annotation.personal.note", values: [.string("c")]),
            ]),
        ]
        let document = try KRDS.decode(KRDS.encode(values))
        #expect(document.values == values)
    }

    @Test func extractsPercentDateAndAnnotations() throws {
        let data = KRDS.encode([
            .field(name: "lpr", values: [.byte(2), .string("AfYEAAAAAAAA:4500"), .long(1_750_000_000_000)]),
            .field(name: "book.info.store", values: [.long(80_000), .double(42.5)]),
            .field(name: "annotation.cache.object", values: [
                .field(name: "annotation.personal.highlight", values: []),
                .field(name: "annotation.personal.note", values: []),
            ]),
        ])
        let progress = try #require(ProgressExtractor.progress(from: [data], metadata: nil))
        #expect(abs((progress.fraction ?? 0) - 0.425) < 0.0001)
        #expect(progress.lastRead == Date(timeIntervalSince1970: 1_750_000_000))
        #expect(progress.position == "AfYEAAAAAAAA:4500")
        #expect(progress.highlights == 1)
        #expect(progress.notes == 1)
        #expect(progress.state == .reading)
    }

    @Test func fallsBackToTextLengthForMOBIPositions() throws {
        let data = KRDS.encode([.field(name: "lpr", values: [.byte(1), .string("7500"), .long(1_700_000_000_000)])])
        var metadata = BookMetadata()
        metadata.textLength = 10_000
        let progress = try #require(ProgressExtractor.progress(from: [data], metadata: metadata))
        #expect(abs((progress.fraction ?? 0) - 0.75) < 0.0001)
    }

    @Test func pdfPagePositions() throws {
        let data = KRDS.encode([.field(name: "lpr", values: [.byte(1), .string("99"), .long(1_700_000_000_000)])])
        var metadata = BookMetadata()
        metadata.pageCount = 100
        let progress = try #require(ProgressExtractor.progress(from: [data], metadata: metadata))
        #expect(progress.isFinished)
    }

    @Test func toleratesTruncatedSidecars() throws {
        var data = KRDS.encode([.field(name: "lpr", values: [.string("12"), .long(1_700_000_000_000)]),
                                .field(name: "font.prefs", values: [.string("Bookerly")])])
        data.removeLast(5)
        let document = try KRDS.decode(data)
        #expect(document.field(named: "lpr") != nil)
    }

    @Test func ignoresNonKRDSFiles() {
        #expect(ProgressExtractor.progress(from: [Data("not a sidecar".utf8)], metadata: nil) == nil)
    }
}

@Suite("Library")
struct LibraryModelTests {
    @Test(arguments: [
        ("The_Hobbit-asin_B000FA5ZEG-type_EBOK-v_0.azw", "The Hobbit"),
        ("Annual Report 2025.pdf", "Annual Report 2025"),
        ("Dune_B00B7NPRY8_EBOK.azw3", "Dune"),
    ])
    func cleansFileNames(fileName: String, expected: String) {
        #expect(Book.cleanTitle(fromFileName: fileName) == expected)
    }

    @Test func everyBookExtensionIsUploadableAsIs() {
        for ext in BookFormat.fileExtensions {
            #expect(BookFormat.isUploadable(URL(fileURLWithPath: "/tmp/book.\(ext)")), "\(ext)")
            #expect(BookFormat.isUploadable(URL(fileURLWithPath: "/tmp/BOOK.\(ext.uppercased())")), "\(ext) uppercase")
        }
        #expect(BookFormat(pathExtension: "epub") == .epub)
        #expect(!BookFormat.isUploadable(URL(fileURLWithPath: "/tmp/photo.jpg")))
        #expect(BookFormat.importTypes.contains(.epub))
    }

    @Test func findsASINInFileName() {
        #expect(Book.asin(inFileName: "Dune_B00B7NPRY8_EBOK.azw3") == "B00B7NPRY8")
        #expect(Book.asin(inFileName: "notes.pdf") == nil)
    }

    @Test func kfxHeuristicFindsTitleAndAuthor() {
        func ion(_ s: String) -> [UInt8] {
            let n = UInt8(s.utf8.count)
            return (n < 14 ? [0x80 | n] : [0x8E, 0x80 | n]) + Array(s.utf8)
        }
        var bytes = Array("CONT".utf8) + [UInt8](repeating: 0, count: 40)
        bytes += [0x8B] + ion("title") + [0x82, 0xB3] + ion("Neuromancer")
        bytes += [0x8B] + ion("author") + [0x82, 0xB3] + ion("William Gibson")
        let metadata = KFXMetadata.read(from: Data(bytes))
        #expect(metadata?.title == "Neuromancer")
        #expect(metadata?.authors == ["William Gibson"])
    }

    @Test func filtersByReadingState() {
        var book = Book(path: "documents/a.pdf", format: .pdf, size: 1, modified: nil, metadata: BookMetadata())
        #expect(LibraryFilter.unread.includes(book))
        book.progress = ReadingProgress(fraction: 0.5)
        #expect(LibraryFilter.reading.includes(book))
        book.progress = ReadingProgress(fraction: 1)
        #expect(LibraryFilter.finished.includes(book))
        #expect(LibraryFilter.pdfs.includes(book))
    }
}
