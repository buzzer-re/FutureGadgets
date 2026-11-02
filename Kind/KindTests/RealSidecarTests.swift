import Foundation
import Testing
@testable import Kind

/// Sidecars with the exact structure a Kindle Paperwhite writes for a sideloaded PDF
/// (timestamps replaced with synthetic values). The `.azw3f` is a stale record from an
/// earlier open; the `.yjf` is the current one the Kindle writes after converting the PDF.
@Suite("Real Kindle sidecars")
struct RealSidecarTests {
    static let currentYJF = Data(base64Encoded: "AAAAAAAasSYCAAAAAAAAAAEBAAAABP4AAANmcHIDAAAVQVd3SkFBQUFBQUFBOjE5OTIyOTQ2Av//////////Av//////////AwEDAf/+AAAScGFnZS5oaXN0b3J5LnN0b3JlAQAAAAD//gAAHXdoaXNwZXJzdG9yZS5taWdyYXRpb24uc3RhdHVzAAAAAP/+AAADbHByBwIDAAAVQVd3SkFBQUFBQUFBOjE5OTIyOTQ2AgAAAZd0INwA/w==")!
    static let staleAZW3F = Data(base64Encoded: "AAAAAAAasSYCAAAAAAAAAAEBAAAABv4AAAt0aW1lci5tb2RlbAIAAAAAAAAAAAIAAAAAAAAFlgIAAAAAAAAADwQ/PRRoqB39AP4AABh0aW1lci5hdmVyYWdlLmNhbGN1bGF0b3IBAAAAAQRAenupOGgitwEAAAABBECJ+DMO5FfwAQAAAAABAAAAAP///gAAA2ZwcgMAAAUxNjc0OAL//////////wL//////////wMBAwH//gAAD2Jvb2suaW5mby5zdG9yZQIAAAAAAAABUwQ/Zc9OfhZ9yP/+AAAScGFnZS5oaXN0b3J5LnN0b3JlAQAAAAD//gAAHXdoaXNwZXJzdG9yZS5taWdyYXRpb24uc3RhdHVzAAAAAP/+AAADbHByBwIDAAAFMTY3NDgCAAABi8/laAD/")!

    @Test func decodesRealFiles() throws {
        let yjf = try KRDS.decode(Self.currentYJF)
        let lpr = try #require(yjf.field(named: "lpr"))
        #expect(ProgressExtractor.positionAndDate(in: lpr).0 == "AWwJAAAAAAAA:19922946")

        let azw3f = try KRDS.decode(Self.staleAZW3F)
        #expect(azw3f.field(named: "timer.model") != nil)
        #expect(azw3f.field(named: "book.info.store") != nil)
    }

    @Test func newestRecordWinsAndPageIsDecoded() throws {
        var metadata = BookMetadata()
        metadata.pageCount = 318
        // Order must not matter: the stale file comes first here.
        let progress = try #require(ProgressExtractor.progress(from: [Self.staleAZW3F, Self.currentYJF], metadata: metadata))
        #expect(progress.position == "AWwJAAAAAAAA:19922946")
        #expect(progress.lastRead == Date(timeIntervalSince1970: 1_750_000_000))
        // 19922946 >> 16 == 304 → page 305 of 318.
        #expect(abs((progress.fraction ?? 0) - 305.0 / 318.0) < 0.0001)
        #expect(progress.state == .reading)
    }

    @Test(arguments: [
        ("The Lighthouse_ A Novel_ Basis for the TV Series _Welcome Home_ (Portuguese Edition)_B00TESTASN_sample.kfx",
         "The Lighthouse: A Novel: Basis for the TV Series Welcome Home (Portuguese Edition)"),
        ("Build-a-tiny-compiler - Unknown.pdf", "Build-a-tiny-compiler"),
        ("Uma Novela de Exemplo - Jane Doe.pdf", "Uma Novela de Exemplo"),
    ])
    func realFileNames(fileName: String, title: String) {
        #expect(Book.cleanTitle(fromFileName: fileName) == title)
    }

    @Test func authorFromFileName() {
        let book = Book(path: "documents/Uma Novela de Exemplo - Jane Doe.pdf", format: .pdf, size: 1, modified: nil, metadata: BookMetadata())
        #expect(book.authorLine == "Jane Doe")
        let unknown = Book(path: "documents/Build-a-tiny-compiler - Unknown.pdf", format: .pdf, size: 1, modified: nil, metadata: BookMetadata())
        #expect(unknown.authorLine.isEmpty)
    }
}
