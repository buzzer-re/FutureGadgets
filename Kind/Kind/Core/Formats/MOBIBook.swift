import Foundation

/// Reads and rewrites MOBI, AZW and AZW3 (KF8) books.
nonisolated enum MOBIBook {
    struct Summary: Sendable {
        var metadata: BookMetadata
        var cover: Data?
    }

    /// Reads metadata (and optionally the cover) touching only the byte ranges needed:
    /// the PDB header, the record table, record 0 and the cover image record.
    static func read(from source: some RandomAccessSource, includeCover: Bool = true) async throws -> Summary {
        let head = try await source.read(offset: 0, length: PalmDatabase.headerSize)
        var reader = ByteReader(head)
        guard head.count == PalmDatabase.headerSize else { throw BinaryError.unexpectedEnd(offset: 0, wanted: 78) }
        try reader.seek(to: 60)
        let type = String(decoding: try reader.read(8), as: UTF8.self)
        guard type == "BOOKMOBI" || type == "TEXtREAd" else { throw BinaryError.invalidMagic("PalmDB type \(type)") }
        try reader.seek(to: 76)
        let count = Int(try reader.u16be())
        guard count > 0 else { throw BinaryError.malformed("no records") }

        let table = try await source.read(offset: PalmDatabase.headerSize, length: count * 8)
        let offsets: [Int] = try (0 ..< count).map { index in
            guard let value = ByteReader(table).peekU32be(at: index * 8) else { throw BinaryError.unexpectedEnd(offset: index * 8, wanted: 4) }
            return Int(value)
        }
        func range(of index: Int) -> Range<Int>? {
            guard index >= 0, index < offsets.count else { return nil }
            let end = index + 1 < offsets.count ? offsets[index + 1] : source.size
            return offsets[index] < end ? offsets[index] ..< end : nil
        }

        guard let record0Range = range(of: 0) else { throw BinaryError.malformed("record 0") }
        let record0 = try await source.read(offset: record0Range.lowerBound, length: record0Range.count)
        let header = try MOBIHeader(record: record0)
        var metadata = header.metadata

        var cover: Data?
        if includeCover, header.firstImageIndex != MOBIHeader.noIndex {
            let offset = header.integer(.coverOffset) ?? header.integer(.thumbnailOffset)
            if let offset, offset != MOBIHeader.noIndex,
               let imageRange = range(of: Int(header.firstImageIndex) + Int(offset)) {
                let data = try await source.read(offset: imageRange.lowerBound, length: imageRange.count)
                if ImageSniffer.isImage(data) { cover = data }
            }
        }
        if metadata.title.isEmpty {
            metadata.title = String(decoding: head.prefix(32), as: UTF8.self)
                .trimmingCharacters(in: .controlCharacters).replacingOccurrences(of: "_", with: " ")
        }
        return Summary(metadata: metadata, cover: cover)
    }

    enum EditError: LocalizedError {
        case drmProtected
        var errorDescription: String? {
            "This book is protected by DRM, so its metadata can't be changed."
        }
    }

    /// Returns a new file with the changes applied to every MOBI header in the file
    /// (the KF7 header and, for combined files, the KF8 header after the boundary).
    static func rewrite(_ data: Data, applying changes: MetadataChanges, cover: Data?, thumbnail: Data?) throws -> Data {
        var database = try PalmDatabase(data: data)
        var header = try MOBIHeader(record: database.records[0].data)
        guard !header.isEncrypted else { throw EditError.drmProtected }

        var headerIndices = [0]
        if let boundary = header.integer(.kf8Boundary), boundary != MOBIHeader.noIndex,
           Int(boundary) < database.records.count,
           (try? MOBIHeader(record: database.records[Int(boundary)].data)) != nil {
            headerIndices.append(Int(boundary))
        }

        for index in headerIndices {
            header = try MOBIHeader(record: database.records[index].data)
            header.apply(changes)
            database.records[index].data = header.serialized()
        }

        // Replace the embedded cover and thumbnail records in place so no record indices move.
        let primary = try MOBIHeader(record: database.records[0].data)
        if primary.firstImageIndex != MOBIHeader.noIndex {
            let base = Int(primary.firstImageIndex)
            if let cover, let offset = primary.integer(.coverOffset), base + Int(offset) < database.records.count {
                database.records[base + Int(offset)].data = cover
            }
            if let thumbnail, let offset = primary.integer(.thumbnailOffset), base + Int(offset) < database.records.count {
                database.records[base + Int(offset)].data = thumbnail
            }
        }

        // Keep the PDB name in sync with the title – it's what older firmware shows as a fallback.
        if let title = changes.title, !title.isEmpty {
            let ascii = title.unicodeScalars.map { $0.isASCII && $0.value >= 0x20 ? Character($0) : "_" }
            let name = Array(String(ascii).replacingOccurrences(of: " ", with: "_").utf8.prefix(31))
            database.header.replaceSubrange(0 ..< 32, with: name + [UInt8](repeating: 0, count: 32 - name.count))
        }
        return database.serialized()
    }
}

nonisolated enum ImageSniffer {
    static func isImage(_ data: Data) -> Bool {
        let bytes = [UInt8](data.prefix(8))
        guard bytes.count >= 4 else { return false }
        return bytes.starts(with: [0xFF, 0xD8, 0xFF]) // JPEG
            || bytes.starts(with: [0x89, 0x50, 0x4E, 0x47]) // PNG
            || bytes.starts(with: Array("GIF8".utf8))
            || bytes.starts(with: Array("BM".utf8))
    }
}
