import AppKit
import Foundation

/// High-level operations on a Kindle's library. All device I/O goes through the
/// `TransferCenter` so the user sees progress and operations never interleave.
struct LibraryActions {
    let device: KindleDevice
    let transfers: TransferCenter

    static let documentsFolder = "documents"

    // MARK: Upload

    func upload(_ urls: [URL]) {
        for url in urls {
            guard BookFormat.isUploadable(url) else { continue }
            let name = url.lastPathComponent
            transfers.enqueue(.upload, name: name) { progress in
                guard let fileSystem = device.fileSystem else { throw KindleError.disconnected }
                let accessing = url.startAccessingSecurityScopedResource()
                defer { if accessing { url.stopAccessingSecurityScopedResource() } }
                let item = try await fileSystem.upload(url, to: Self.documentsFolder, name: name, progress: progress)
                await device.ingest(item)
            }
        }
    }

    // MARK: Delete

    func delete(_ books: [Book]) {
        for book in books {
            transfers.enqueue(.delete, name: book.title) { _ in
                guard let fileSystem = device.fileSystem else { throw KindleError.disconnected }
                try await fileSystem.delete(book.path)
                if let sidecar = book.sidecarPath { try? await fileSystem.delete(sidecar) }
                await CoverStore.shared.remove(device.coverKey(for: book))
                device.remove([book.id])
                await device.refreshStorage()
            }
        }
    }

    // MARK: Export

    func export(_ books: [Book], to folder: URL) {
        for book in books {
            transfers.enqueue(.download, name: book.title) { progress in
                guard let fileSystem = device.fileSystem else { throw KindleError.disconnected }
                let destination = Self.uniqueURL(in: folder, name: book.fileName)
                try await fileSystem.download(book.path, to: destination, progress: progress)
            }
        }
    }

    nonisolated static func uniqueURL(in folder: URL, name: String) -> URL {
        var candidate = folder.appendingPathComponent(name)
        let base = (name as NSString).deletingPathExtension
        let ext = (name as NSString).pathExtension
        var counter = 2
        while FileManager.default.fileExists(atPath: candidate.path) {
            candidate = folder.appendingPathComponent("\(base) \(counter)").appendingPathExtension(ext)
            counter += 1
        }
        return candidate
    }

    // MARK: Metadata

    /// Downloads the book, rewrites its metadata locally, and sends it back.
    /// Working on a copy means a failed write can never corrupt the book on the Kindle.
    func applyMetadata(_ changes: MetadataChanges, to book: Book, insertPDFCoverPage: Bool) {
        transfers.enqueue(.edit, name: book.title) { progress in
            guard let fileSystem = device.fileSystem else { throw KindleError.disconnected }
            let workspace = FileManager.default.temporaryDirectory.appendingPathComponent("kind-edit-\(UUID().uuidString)")
            try FileManager.default.createDirectory(at: workspace, withIntermediateDirectories: true)
            defer { try? FileManager.default.removeItem(at: workspace) }
            let local = workspace.appendingPathComponent(book.fileName)

            try await fileSystem.download(book.path, to: local) { done, total in progress(done / 3, total) }

            let cover = changes.coverImage.flatMap { CoverEncoder.encode($0, maxDimension: 1600) }
            let thumbnail = changes.coverImage.flatMap { CoverEncoder.encode($0, maxDimension: 330, quality: 0.8) }
            try await Task.detached(priority: .userInitiated) {
                switch book.format {
                case .mobi, .azw, .azw3:
                    let data = try Data(contentsOf: local)
                    let rewritten = try MOBIBook.rewrite(data, applying: changes, cover: cover, thumbnail: thumbnail)
                    try rewritten.write(to: local, options: .atomic)
                case .pdf:
                    var pdfChanges = changes
                    pdfChanges.coverImage = cover
                    try PDFBook.rewrite(url: local, applying: pdfChanges, insertCoverPage: insertPDFCoverPage)
                default:
                    throw KindleError.unsupported("Editing \(book.format.label) files isn't supported.")
                }
            }.value

            let item = try await fileSystem.upload(local, to: book.folder, name: book.fileName) { done, total in
                progress(total / 3 + done * 2 / 3, total)
            }

            // Store books show the Kindle-generated thumbnail in the home screen; refresh it too.
            if let thumbnail, let asin = book.asin {
                let cdeType = book.metadata.cdeType.isEmpty ? "EBOK" : book.metadata.cdeType
                let thumbURL = workspace.appendingPathComponent("thumbnail_\(asin)_\(cdeType)_portrait.jpg")
                try thumbnail.write(to: thumbURL)
                _ = try? await fileSystem.upload(thumbURL, to: "system/thumbnails", name: thumbURL.lastPathComponent, progress: nil)
            }

            await CoverStore.shared.remove(device.coverKey(for: book))
            var updated = book
            updated.size = item.size
            updated.modified = item.modified
            updated.isEnriched = false
            updated.hasEmbeddedCover = false
            device.update(updated)
            await device.ingestPreservingProgress(updated)
        }
    }
}

extension KindleDevice {
    /// Re-parses an edited book but keeps its sidecar-derived reading progress.
    func ingestPreservingProgress(_ book: Book) async {
        guard let fileSystem else { return }
        let scanner = LibraryScanner(fileSystem: fileSystem, deviceID: id, cache: cache, isLocal: isMassStorage)
        var enriched = await scanner.enrich(book, sidecars: [])
        enriched.progress = book.progress
        update(enriched)
        await cache.save()
    }
}
