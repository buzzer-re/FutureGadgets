import Foundation
import Observation
import os
import SwiftUI

/// A connected Kindle and its library.
@Observable
final class KindleDevice: Identifiable {
    enum Connection: Equatable {
        case massStorage(URL)
        case mtp(registryID: UInt64)

        var label: String {
            switch self {
            case .massStorage: "USB Drive"
            case .mtp: "MTP"
            }
        }
    }

    enum Phase: Equatable {
        case connecting
        case listing
        case enriching(done: Int, total: Int)
        case ready
        case failed(String)

        var isBusy: Bool {
            switch self {
            case .connecting, .listing, .enriching: true
            case .ready, .failed: false
            }
        }
    }

    static let busyMessage = "The Kindle is in use by another app. Quit Android File Transfer, OpenMTP or Calibre and Kind will connect automatically."

    let id: String
    var name: String
    var model: String
    var serialNumber: String
    let connection: Connection
    var phase: Phase = .connecting
    var storage: StorageInfo?
    var batteryLevel: Int?
    var books: [Book] = []
    var lastSynced: Date?

    @ObservationIgnored private(set) var fileSystem: (any KindleFileSystem)?
    @ObservationIgnored private var sidecars: [String: [RemoteItem]] = [:]
    @ObservationIgnored private var scanTask: Task<Void, Never>?
    @ObservationIgnored private(set) lazy var cache = MetadataCache(deviceID: id)

    init(id: String, name: String, model: String, serialNumber: String, connection: Connection) {
        self.id = id
        self.name = name
        self.model = model
        self.serialNumber = serialNumber
        self.connection = connection
    }

    var isMassStorage: Bool {
        if case .massStorage = connection { return true }
        return false
    }

    var volumeURL: URL? {
        if case let .massStorage(url) = connection { return url }
        return nil
    }

    func attach(_ fileSystem: any KindleFileSystem) {
        self.fileSystem = fileSystem
        refresh()
    }

    func book(withID id: Book.ID) -> Book? { books.first { $0.id == id } }

    func coverKey(for book: Book) -> String { CoverStore.key(device: id, contentKey: book.contentKey) }

    // MARK: Scanning

    func refresh() {
        guard let fileSystem else { return }
        scanTask?.cancel()
        let scanner = LibraryScanner(fileSystem: fileSystem, deviceID: id, cache: cache, isLocal: isMassStorage)
        scanTask = Task { [weak self] in
            await self?.scan(with: scanner)
        }
    }

    private func scan(with scanner: LibraryScanner) async {
        if books.isEmpty { phase = .listing }
        async let storageInfo = try? scanner.fileSystem.storage()
        do {
            let listing = try await scanner.list()
            guard !Task.isCancelled else { return }
            sidecars = listing.sidecarFiles
            // Keep progress we already know while the fresh pass runs, so nothing flickers.
            let previous = Dictionary(books.map { ($0.id, $0) }, uniquingKeysWith: { a, _ in a })
            books = listing.books.map { book in
                var book = book
                if book.progress == nil { book.progress = previous[book.id]?.progress }
                return book
            }
            storage = await storageInfo
            if let mtp = scanner.fileSystem as? MTPFileSystem { batteryLevel = await mtp.batteryLevel() }

            let total = books.count
            phase = .enriching(done: 0, total: total)
            try await enrich(listing.books, with: scanner, total: total)
            await cache.save(keeping: Set(books.map(\.contentKey)))
            lastSynced = .now
            phase = .ready
            Logger(subsystem: "app.kind", category: "library").notice("Library ready: \(self.books.count) books on \(self.name, privacy: .public); progress: \(self.books.map { "\($0.title.prefix(24))=\($0.progress?.fraction.map { Int($0 * 100) } ?? -1)%" }.joined(separator: ", "), privacy: .public)")
        } catch is CancellationError {
            return
        } catch {
            Logger(subsystem: "app.kind", category: "library").error("Scan failed: \(error.localizedDescription, privacy: .public)")
            phase = .failed(error.localizedDescription)
        }
    }

    /// Parses books with bounded concurrency and publishes results in batches, so a large
    /// library streams in smoothly instead of re-rendering the grid once per book.
    private func enrich(_ pending: [Book], with scanner: LibraryScanner, total: Int) async throws {
        let width = isMassStorage ? 4 : 2
        let sidecars = self.sidecars
        var done = 0
        var buffer: [Book] = []
        var lastFlush = ContinuousClock.now

        try await withThrowingTaskGroup(of: Book.self) { group in
            var iterator = pending.makeIterator()
            for _ in 0 ..< width {
                guard let book = iterator.next() else { break }
                group.addTask { await scanner.enrich(book, sidecars: sidecars[book.sidecarPath ?? ""] ?? []) }
            }
            while let enriched = try await group.next() {
                try Task.checkCancellation()
                buffer.append(enriched)
                done += 1
                if buffer.count >= 16 || ContinuousClock.now - lastFlush > .milliseconds(250) || done == total {
                    apply(buffer)
                    buffer.removeAll()
                    lastFlush = .now
                    phase = .enriching(done: done, total: total)
                }
                if let book = iterator.next() {
                    group.addTask { await scanner.enrich(book, sidecars: sidecars[book.sidecarPath ?? ""] ?? []) }
                }
            }
        }
        apply(buffer)
    }

    private func apply(_ batch: [Book]) {
        guard !batch.isEmpty else { return }
        var updated = books
        let indices = Dictionary(updated.enumerated().map { ($1.id, $0) }, uniquingKeysWith: { a, _ in a })
        for book in batch {
            if let index = indices[book.id] { updated[index] = book } else { updated.append(book) }
        }
        withAnimation(.smooth(duration: 0.3)) { books = updated }
    }

    func update(_ book: Book) {
        if let index = books.firstIndex(where: { $0.id == book.id }) {
            books[index] = book
        } else {
            books.append(book)
        }
    }

    func remove(_ ids: Set<Book.ID>) {
        books.removeAll { ids.contains($0.id) }
    }

    func refreshStorage() async {
        guard let fileSystem else { return }
        storage = try? await fileSystem.storage()
    }

    /// Enriches a single, newly uploaded or edited item without rescanning everything.
    func ingest(_ item: RemoteItem) async {
        guard let fileSystem, let format = BookFormat(pathExtension: item.pathExtension) else { return }
        let scanner = LibraryScanner(fileSystem: fileSystem, deviceID: id, cache: cache, isLocal: isMassStorage)
        var book = Book(path: item.path, format: format, size: item.size, modified: item.modified, metadata: BookMetadata())
        update(book)
        book = await scanner.enrich(book, sidecars: [])
        update(book)
        await cache.save()
        await refreshStorage()
    }

    func disconnect() async {
        scanTask?.cancel()
        if let mtp = fileSystem as? MTPFileSystem { await mtp.disconnect() }
        fileSystem = nil
    }
}
