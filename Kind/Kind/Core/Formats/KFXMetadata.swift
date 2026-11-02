import Foundation

/// Best-effort title/author extraction from KFX containers (`CONT` magic).
///
/// KFX stores metadata as Amazon Ion binary inside a `book_metadata` entity, as key/value
/// structs such as `{ key: "title", value: "Dune" }`. Keys and values are plain Ion strings,
/// so we can locate them without a full Ion + symbol-table implementation.
nonisolated enum KFXMetadata {
    static func isKFX(_ head: Data) -> Bool {
        head.count >= 4 && head.prefix(4) == Data("CONT".utf8)
    }

    static func read(from data: Data) -> BookMetadata? {
        let bytes = [UInt8](data)
        var metadata = BookMetadata()
        metadata.title = value(for: "title", in: bytes) ?? ""
        if let author = value(for: "author", in: bytes) { metadata.authors = [author] }
        metadata.publisher = value(for: "publisher", in: bytes) ?? ""
        metadata.asin = value(for: "ASIN", in: bytes) ?? value(for: "asin", in: bytes) ?? ""
        metadata.cdeType = value(for: "cde_content_type", in: bytes) ?? ""
        metadata.language = value(for: "language", in: bytes) ?? ""
        metadata.summary = value(for: "description", in: bytes) ?? ""
        return metadata.title.isEmpty ? nil : metadata
    }

    /// Finds an Ion string equal to `key` and returns the next Ion string within a few bytes.
    private static func value(for key: String, in bytes: [UInt8]) -> String? {
        let keyBytes = Array(key.utf8)
        guard keyBytes.count < 14 else { return nil }
        let needle = [0x80 | UInt8(keyBytes.count)] + keyBytes
        var index = 0
        while let found = firstIndex(of: needle, in: bytes, from: index) {
            let after = found + needle.count
            for start in after ..< min(bytes.count, after + 6) {
                if let string = ionString(at: start, in: bytes), !string.isEmpty, string != key {
                    return string
                }
            }
            index = found + 1
        }
        return nil
    }

    private static func ionString(at offset: Int, in bytes: [UInt8]) -> String? {
        let type = bytes[offset]
        guard type >> 4 == 0x8 else { return nil }
        var length = Int(type & 0x0F)
        var position = offset + 1
        if length == 14 { // VarUInt length follows
            length = 0
            while position < bytes.count {
                let byte = bytes[position]
                position += 1
                length = (length << 7) | Int(byte & 0x7F)
                if byte & 0x80 != 0 { break }
                if length > 1 << 16 { return nil }
            }
        } else if length == 15 {
            return nil
        }
        guard length > 0, position + length <= bytes.count else { return nil }
        guard let string = String(bytes: bytes[position ..< position + length], encoding: .utf8),
              !string.unicodeScalars.contains(where: { $0.properties.generalCategory == .control && $0 != "\n" }) else { return nil }
        return string
    }

    private static func firstIndex(of needle: [UInt8], in haystack: [UInt8], from start: Int) -> Int? {
        guard needle.count <= haystack.count else { return nil }
        var i = start
        while i <= haystack.count - needle.count {
            if haystack[i] == needle[0], Array(haystack[i ..< i + needle.count]) == needle { return i }
            i += 1
        }
        return nil
    }
}
