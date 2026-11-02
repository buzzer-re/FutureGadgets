import Foundation

/// Builds the library from a Kindle's file system in two passes:
/// 1. a fast listing that yields every book immediately (cached metadata or file name), then
/// 2. an enrichment pass that parses headers, covers and sidecars book by book.
nonisolated struct LibraryScanner: Sendable {
    let fileSystem: any KindleFileSystem
    let deviceID: String
    let cache: MetadataCache
    let isLocal: Bool

    static let ignoredFolders: Set<String> = ["dictionaries", ".active content data", "My Clippings"]
    static let maxSidecarSize = 4 << 20

    struct Listing: Sendable {
        var books: [Book]
        var sidecarFiles: [String: [RemoteItem]]
    }

    // MARK: Pass 1

    func list() async throws -> Listing {
        let items = try await fileSystem.list("documents")
        let thumbnails = (try? await fileSystem.list("system/thumbnails")) ?? []
        var thumbnailsByASIN: [String: String] = [:]
        for item in thumbnails where !item.isDirectory {
            if let asin = Book.asin(inFileName: item.name) { thumbnailsByASIN[asin] = item.path }
        }

        var sidecarFiles: [String: [RemoteItem]] = [:]
        var books: [Book] = []
        for item in items {
            let components = item.path.split(separator: "/")
            if let sdrIndex = components.firstIndex(where: { $0.hasSuffix(".sdr") }) {
                if !item.isDirectory {
                    let folder = components[...sdrIndex].joined(separator: "/")
                    sidecarFiles[folder, default: []].append(item)
                }
                continue
            }
            guard !item.isDirectory, !item.name.hasPrefix("."),
                  let format = BookFormat(pathExtension: item.pathExtension),
                  !components.contains(where: { Self.ignoredFolders.contains(String($0)) }) else { continue }

            var book = Book(path: item.path, format: format, size: item.size, modified: item.modified,
                            metadata: BookMetadata())
            let sidecar = (item.path as NSString).deletingPathExtension + ".sdr"
            book.sidecarPath = sidecar
            if let (metadata, hasCover) = await cache.lookup(book.contentKey) {
                book.metadata = metadata
                book.hasEmbeddedCover = hasCover
                book.isEnriched = true
            }
            if let asin = book.asin, let thumb = thumbnailsByASIN[asin] { book.thumbnailPath = thumb }
            books.append(book)
        }
        for index in books.indices where sidecarFiles[books[index].sidecarPath ?? ""] == nil {
            books[index].sidecarPath = nil
        }
        return Listing(books: books, sidecarFiles: sidecarFiles)
    }

    // MARK: Pass 2

    /// Parses metadata/cover when not cached and always refreshes reading progress.
    func enrich(_ book: Book, sidecars: [RemoteItem]) async -> Book {
        var book = book
        let coverKey = CoverStore.key(device: deviceID, contentKey: book.contentKey)
        if !book.isEnriched {
            if let (metadata, cover) = await parse(book) {
                book.metadata = metadata
                if let cover { book.hasEmbeddedCover = await CoverStore.shared.store(cover, for: coverKey) }
            }
            // Store books without an embedded cover still have the Kindle-generated thumbnail.
            if !book.hasEmbeddedCover, let thumb = book.thumbnailPath,
               let data = try? await fileSystem.read(thumb) {
                book.hasEmbeddedCover = await CoverStore.shared.store(data, for: coverKey)
            }
            book.isEnriched = true
            await cache.store(book.metadata, hasCover: book.hasEmbeddedCover, for: book.contentKey)
        }
        book.progress = await progress(for: book, sidecars: sidecars)
        return book
    }

    func parse(_ book: Book) async -> (BookMetadata, Data?)? {
        do {
            switch book.format {
            case .mobi, .azw, .azw3:
                let source = RemoteFileSource(fileSystem: fileSystem, item: item(for: book))
                if let summary = try? await MOBIBook.read(from: source) {
                    return (summary.metadata, summary.cover)
                }
                return try await parseKFX(book)
            case .kfx:
                return try await parseKFX(book)
            case .pdf:
                return try await withLocalCopy(of: book) { url in
                    PDFBook.read(url: url).map { ($0.metadata, $0.cover) }
                }
            case .txt, .docx, .epub:
                return nil
            }
        } catch {
            return nil
        }
    }

    private func parseKFX(_ book: Book) async throws -> (BookMetadata, Data?)? {
        // Metadata lives near the start of the container; avoid pulling whole books over USB.
        let head = try await fileSystem.read(book.path, offset: 0, length: min(book.size, 2 << 20))
        return KFXMetadata.read(from: head).map { ($0, nil) }
    }

    private func item(for book: Book) -> RemoteItem {
        RemoteItem(path: book.path, size: book.size, modified: book.modified, isDirectory: false)
    }

    /// PDFKit needs a real file. Mass-storage books are opened in place; MTP books are
    /// fetched to a temporary file first.
    private func withLocalCopy<T>(of book: Book, _ body: (URL) throws -> T?) async throws -> T? {
        if isLocal, let local = fileSystem as? MassStorageFileSystem {
            return try body(local.root.appendingPathComponent(book.path))
        }
        let temp = FileManager.default.temporaryDirectory.appendingPathComponent("kind-\(UUID().uuidString).\(book.format.rawValue)")
        defer { try? FileManager.default.removeItem(at: temp) }
        try await fileSystem.download(book.path, to: temp, progress: nil)
        return try body(temp)
    }

    func progress(for book: Book, sidecars: [RemoteItem]) async -> ReadingProgress? {
        var blobs: [Data] = []
        for file in sidecars where file.size > 8 && file.size < Self.maxSidecarSize {
            if let data = try? await fileSystem.read(file.path) { blobs.append(data) }
        }
        return ProgressExtractor.progress(from: blobs, metadata: book.metadata)
    }
}
