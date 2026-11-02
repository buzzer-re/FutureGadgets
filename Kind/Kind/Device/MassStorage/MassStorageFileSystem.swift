import Foundation

/// Kindles up to ~2021 expose their storage as a FAT32 USB volume. Everything is plain file I/O.
nonisolated final class MassStorageFileSystem: KindleFileSystem {
    let root: URL

    init(root: URL) {
        self.root = root
    }

    private func url(_ path: String) -> URL {
        path.isEmpty ? root : root.appendingPathComponent(path)
    }

    func list(_ path: String) async throws -> [RemoteItem] {
        let base = url(path)
        let keys: [URLResourceKey] = [.isDirectoryKey, .fileSizeKey, .contentModificationDateKey]
        guard let enumerator = FileManager.default.enumerator(at: base, includingPropertiesForKeys: keys,
                                                              options: [.skipsHiddenFiles]) else {
            throw KindleError.notFound(path)
        }
        let rootPath = root.standardizedFileURL.path
        var items: [RemoteItem] = []
        while let next = enumerator.nextObject() as? URL {
            let values = try? next.resourceValues(forKeys: Set(keys))
            var relative = String(next.standardizedFileURL.path.dropFirst(rootPath.count))
            if relative.hasPrefix("/") { relative.removeFirst() }
            items.append(RemoteItem(path: relative, size: values?.fileSize ?? 0,
                                    modified: values?.contentModificationDate,
                                    isDirectory: values?.isDirectory ?? false))
        }
        return items
    }

    func read(_ path: String) async throws -> Data {
        try Data(contentsOf: url(path), options: .mappedIfSafe)
    }

    func read(_ path: String, offset: Int, length: Int) async throws -> Data {
        let handle = try FileHandle(forReadingFrom: url(path))
        defer { try? handle.close() }
        try handle.seek(toOffset: UInt64(offset))
        return try handle.read(upToCount: length) ?? Data()
    }

    func download(_ path: String, to destination: URL, progress: TransferProgress?) async throws {
        try? FileManager.default.removeItem(at: destination)
        try await copy(from: url(path), to: destination, progress: progress)
    }

    func upload(_ source: URL, to directory: String, name: String, progress: TransferProgress?) async throws -> RemoteItem {
        let size = Int64((try? source.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
        if let info = try? await storage(), info.free < size { throw KindleError.notEnoughSpace }
        let folder = url(directory)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let destination = folder.appendingPathComponent(name)
        let temp = folder.appendingPathComponent(".\(name).kindpart")
        try? FileManager.default.removeItem(at: temp)
        try await copy(from: source, to: temp, progress: progress)
        if FileManager.default.fileExists(atPath: destination.path) {
            _ = try FileManager.default.replaceItemAt(destination, withItemAt: temp)
        } else {
            try FileManager.default.moveItem(at: temp, to: destination)
        }
        let path = directory.isEmpty ? name : "\(directory)/\(name)"
        return RemoteItem(path: path, size: Int(size), modified: .now, isDirectory: false)
    }

    /// Chunked copy so we can report progress for large PDFs.
    private func copy(from source: URL, to destination: URL, progress: TransferProgress?) async throws {
        let total = Int64((try? source.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
        FileManager.default.createFile(atPath: destination.path, contents: nil)
        let input = try FileHandle(forReadingFrom: source)
        let output = try FileHandle(forWritingTo: destination)
        defer { try? input.close(); try? output.close() }
        var done: Int64 = 0
        while let chunk = try input.read(upToCount: 1 << 20), !chunk.isEmpty {
            try Task.checkCancellation()
            try output.write(contentsOf: chunk)
            done += Int64(chunk.count)
            progress?(done, total)
        }
        try output.synchronize()
    }

    func delete(_ path: String) async throws {
        try FileManager.default.removeItem(at: url(path))
    }

    func storage() async throws -> StorageInfo {
        let values = try root.resourceValues(forKeys: [.volumeTotalCapacityKey, .volumeAvailableCapacityKey])
        return StorageInfo(capacity: Int64(values.volumeTotalCapacity ?? 0),
                           free: Int64(values.volumeAvailableCapacity ?? 0))
    }
}
