import AppKit
import Foundation
import PDFKit

/// PDF metadata via PDFKit. Covers are rendered from the first page, which is exactly
/// what the Kindle shows in its library.
nonisolated enum PDFBook {
    struct Summary: Sendable {
        var metadata: BookMetadata
        var cover: Data?
    }

    static func read(url: URL, includeCover: Bool = true) -> Summary? {
        guard let document = PDFDocument(url: url) else { return nil }
        var metadata = BookMetadata()
        let attributes = document.documentAttributes ?? [:]
        metadata.title = (attributes[PDFDocumentAttribute.titleAttribute] as? String) ?? ""
        if let author = attributes[PDFDocumentAttribute.authorAttribute] as? String, !author.isEmpty {
            metadata.authors = author.components(separatedBy: CharacterSet(charactersIn: ";&"))
                .map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        }
        metadata.summary = (attributes[PDFDocumentAttribute.subjectAttribute] as? String) ?? ""
        metadata.publisher = (attributes[PDFDocumentAttribute.producerAttribute] as? String).flatMap(publisherIfMeaningful) ?? ""
        if let keywords = attributes[PDFDocumentAttribute.keywordsAttribute] {
            metadata.subjects = (keywords as? [String]) ?? ((keywords as? String)?.components(separatedBy: ",") ?? [])
                .map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        }
        if let date = attributes[PDFDocumentAttribute.creationDateAttribute] as? Date {
            metadata.publishedDate = date.formatted(.iso8601.year().month().day())
        }
        metadata.pageCount = document.pageCount
        metadata.isDRMProtected = document.isEncrypted && document.isLocked

        let cover = includeCover ? document.page(at: 0).flatMap { renderThumbnail(of: $0) } : nil
        return Summary(metadata: metadata, cover: cover)
    }

    /// PDF "Producer" is usually the generating software, which isn't worth showing as publisher.
    private static func publisherIfMeaningful(_ producer: String) -> String? {
        let software = ["pdf", "acrobat", "word", "quartz", "latex", "tex", "ghostscript", "distiller", "office", "skia", "itext", "cairo"]
        return software.contains(where: producer.lowercased().contains) ? nil : producer
    }

    static func renderThumbnail(of page: PDFPage, width: CGFloat = 600) -> Data? {
        let bounds = page.bounds(for: .cropBox)
        guard bounds.width > 0 else { return nil }
        let size = CGSize(width: width, height: width * bounds.height / bounds.width)
        let image = page.thumbnail(of: size, for: .cropBox)
        return image.jpegData(quality: 0.85)
    }

    /// Writes metadata into the PDF's info dictionary. If a cover is supplied it is inserted as
    /// a new first page, since that is what the Kindle uses as the cover.
    static func rewrite(url: URL, applying changes: MetadataChanges, insertCoverPage: Bool) throws {
        guard let document = PDFDocument(url: url) else { throw CocoaError(.fileReadCorruptFile) }
        guard !document.isLocked else { throw CocoaError(.fileWriteNoPermission) }
        var attributes = document.documentAttributes ?? [:]
        if let title = changes.title { attributes[PDFDocumentAttribute.titleAttribute] = title }
        if let authors = changes.authors { attributes[PDFDocumentAttribute.authorAttribute] = authors.joined(separator: "; ") }
        if let summary = changes.summary { attributes[PDFDocumentAttribute.subjectAttribute] = summary }
        if let subjects = changes.subjects { attributes[PDFDocumentAttribute.keywordsAttribute] = subjects }
        document.documentAttributes = attributes

        if insertCoverPage, let data = changes.coverImage, let image = NSImage(data: data),
           let page = PDFPage(image: image) {
            if let first = document.page(at: 0) {
                // Match the page size of the book so the Kindle doesn't re-zoom on page one.
                let target = first.bounds(for: .mediaBox)
                let pageImage = image.fitted(in: target.size)
                document.insert(PDFPage(image: pageImage) ?? page, at: 0)
            } else {
                document.insert(page, at: 0)
            }
        }

        let temp = url.deletingLastPathComponent().appendingPathComponent(".kind-\(UUID().uuidString).pdf")
        guard document.write(to: temp) else { throw CocoaError(.fileWriteUnknown) }
        _ = try FileManager.default.replaceItemAt(url, withItemAt: temp)
    }
}

nonisolated extension NSImage {
    func jpegData(quality: CGFloat) -> Data? {
        guard let cg = cgImage(forProposedRect: nil, context: nil, hints: nil) else { return nil }
        let rep = NSBitmapImageRep(cgImage: cg)
        return rep.representation(using: .jpeg, properties: [.compressionFactor: quality])
    }

    /// Scales the image to fit `maxSize` preserving aspect ratio (never upscales beyond 1×).
    func resized(maxDimension: CGFloat) -> NSImage {
        let scale = min(1, maxDimension / max(size.width, size.height))
        let newSize = CGSize(width: (size.width * scale).rounded(), height: (size.height * scale).rounded())
        return NSImage(size: newSize, flipped: false) { rect in
            self.draw(in: rect)
            return true
        }
    }

    /// Letterboxes the image on a white page of the given size.
    func fitted(in pageSize: CGSize) -> NSImage {
        NSImage(size: pageSize, flipped: false) { rect in
            NSColor.white.setFill()
            rect.fill()
            let scale = min(rect.width / self.size.width, rect.height / self.size.height)
            let drawSize = CGSize(width: self.size.width * scale, height: self.size.height * scale)
            let origin = CGPoint(x: (rect.width - drawSize.width) / 2, y: (rect.height - drawSize.height) / 2)
            self.draw(in: CGRect(origin: origin, size: drawSize))
            return true
        }
    }
}

nonisolated enum CoverEncoder {
    /// Kindle covers look best at ~1600px tall; thumbnails at ~330px.
    static func encode(_ data: Data, maxDimension: CGFloat, quality: CGFloat = 0.88) -> Data? {
        guard let image = NSImage(data: data) else { return nil }
        let resized = image.resized(maxDimension: maxDimension)
        guard let tiff = resized.tiffRepresentation, let rep = NSBitmapImageRep(data: tiff) else { return nil }
        return rep.representation(using: .jpeg, properties: [.compressionFactor: quality])
    }
}
