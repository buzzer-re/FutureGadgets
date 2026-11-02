import Foundation

/// A file or folder on the Kindle, addressed by a POSIX-style path relative to the device
/// root (for example `documents/Dune.azw3`).
nonisolated struct RemoteItem: Hashable, Sendable, Identifiable {
    var path: String
    var size: Int
    var modified: Date?
    var isDirectory: Bool

    var id: String { path }
    var name: String { (path as NSString).lastPathComponent }
    var parent: String { (path as NSString).deletingLastPathComponent }
    var pathExtension: String { (path as NSString).pathExtension }
}

nonisolated struct StorageInfo: Hashable, Sendable {
    var capacity: Int64
    var free: Int64
    var used: Int64 { max(0, capacity - free) }
    var usedFraction: Double { capacity > 0 ? Double(used) / Double(capacity) : 0 }
}

/// Progress callback: bytes transferred so far, total bytes.
typealias TransferProgress = @Sendable (Int64, Int64) -> Void

/// Everything Kind needs from a connected Kindle, regardless of whether it is mounted as a
/// USB mass-storage volume (older models) or speaks MTP (Kindles from 2021 onward).
nonisolated protocol KindleFileSystem: AnyObject, Sendable {
    /// Recursively lists everything below `path`.
    func list(_ path: String) async throws -> [RemoteItem]
    func read(_ path: String) async throws -> Data
    func read(_ path: String, offset: Int, length: Int) async throws -> Data
    func download(_ path: String, to destination: URL, progress: TransferProgress?) async throws
    /// Uploads `source` into the folder at `directory`, returning the new item.
    @discardableResult
    func upload(_ source: URL, to directory: String, name: String, progress: TransferProgress?) async throws -> RemoteItem
    func delete(_ path: String) async throws
    func storage() async throws -> StorageInfo
}

/// Adapts a file on the Kindle to the byte-range reader used by the parsers.
nonisolated struct RemoteFileSource: RandomAccessSource {
    let fileSystem: any KindleFileSystem
    let item: RemoteItem
    var size: Int { item.size }

    func read(offset: Int, length: Int) async throws -> Data {
        try await fileSystem.read(item.path, offset: offset, length: min(length, max(0, size - offset)))
    }
}

nonisolated enum KindleError: LocalizedError {
    case notFound(String)
    case notEnoughSpace
    case disconnected
    case unsupported(String)

    var errorDescription: String? {
        switch self {
        case let .notFound(path): "“\(path)” is no longer on the Kindle."
        case .notEnoughSpace: "There isn't enough free space on the Kindle."
        case .disconnected: "The Kindle was disconnected."
        case let .unsupported(what): what
        }
    }
}
