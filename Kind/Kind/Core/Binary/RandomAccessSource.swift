import Foundation

/// Anything that can serve byte ranges of a file: a local file, an in-memory buffer or a
/// file living on a Kindle reached over MTP. Parsers read only the ranges they need, which
/// keeps scanning a device over USB fast.
nonisolated protocol RandomAccessSource: Sendable {
    var size: Int { get }
    func read(offset: Int, length: Int) async throws -> Data
}

nonisolated struct DataSource: RandomAccessSource {
    let data: Data
    var size: Int { data.count }

    func read(offset: Int, length: Int) async throws -> Data {
        guard offset >= 0, offset <= data.count else { throw BinaryError.unexpectedEnd(offset: offset, wanted: length) }
        let end = min(data.count, offset + length)
        return data.subdata(in: data.startIndex + offset ..< data.startIndex + end)
    }
}

nonisolated struct FileSource: RandomAccessSource {
    let url: URL
    let size: Int

    init(url: URL) throws {
        self.url = url
        let values = try url.resourceValues(forKeys: [.fileSizeKey])
        self.size = values.fileSize ?? 0
    }

    func read(offset: Int, length: Int) async throws -> Data {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        try handle.seek(toOffset: UInt64(offset))
        return try handle.read(upToCount: length) ?? Data()
    }
}
