import Foundation

/// EXTH record identifiers used by Kindle books.
nonisolated enum EXTHType: UInt32 {
    case author = 100
    case publisher = 101
    case description = 103
    case isbn = 104
    case subject = 105
    case publishingDate = 106
    case asin = 113
    case kf8Boundary = 121
    case coverOffset = 201
    case thumbnailOffset = 202
    case hasFakeCover = 203
    case cdeType = 501
    case updatedTitle = 503
    case asin2 = 504
    case language = 524
}

/// Record 0 of a MOBI/KF8 section: PalmDOC header, MOBI header, EXTH block and full title.
nonisolated struct MOBIHeader {
    struct EXTHRecord: Equatable {
        var type: UInt32
        var data: Data
    }

    static let noIndex: UInt32 = 0xFFFF_FFFF

    /// PalmDOC header followed by the MOBI header, verbatim.
    private(set) var headerBytes: [UInt8]
    var exth: [EXTHRecord]
    var fullName: Data
    /// Bytes after the full name that some generators use as padding. Preserved on rewrite.
    private var trailingPadding: Int

    var compression: UInt16 { UInt16(headerBytes[0]) << 8 | UInt16(headerBytes[1]) }
    var textLength: Int { Int(field(at: 4)) }
    var mobiType: UInt32 { field(at: 24) }
    var textEncoding: UInt32 { field(at: 28) }
    var fileVersion: UInt32 { field(at: 36) }
    var firstImageIndex: UInt32 { headerBytes.count >= 112 ? field(at: 108) : Self.noIndex }
    var drmOffset: UInt32 { headerBytes.count >= 172 ? field(at: 168) : Self.noIndex }
    var isEncrypted: Bool {
        let encryption = UInt16(headerBytes[12]) << 8 | UInt16(headerBytes[13])
        return encryption != 0 || (drmOffset != Self.noIndex && drmOffset != 0)
    }

    var stringEncoding: String.Encoding { textEncoding == 65001 ? .utf8 : .windowsCP1252 }

    init(record: Data) throws {
        let reader = ByteReader(record)
        guard record.count >= 24, String(decoding: record[record.startIndex + 16 ..< record.startIndex + 20], as: UTF8.self) == "MOBI" else {
            throw BinaryError.invalidMagic("MOBI")
        }
        guard let headerLength = reader.peekU32be(at: 20).map(Int.init), 16 + headerLength <= record.count else {
            throw BinaryError.malformed("MOBI header length")
        }
        let bytes = reader.bytes
        headerBytes = Array(bytes[0 ..< 16 + headerLength])

        exth = []
        let exthFlags = headerBytes.count >= 132 ? reader.peekU32be(at: 128) ?? 0 : 0
        var exthEnd = 16 + headerLength
        if exthFlags & 0x40 != 0, bytes.count >= exthEnd + 12,
           String(decoding: bytes[exthEnd ..< exthEnd + 4], as: UTF8.self) == "EXTH" {
            var exthReader = ByteReader(bytes: bytes, offset: exthEnd + 4)
            let exthLength = Int(try exthReader.u32be())
            let count = Int(try exthReader.u32be())
            for _ in 0 ..< count {
                let type = try exthReader.u32be()
                let length = Int(try exthReader.u32be())
                guard length >= 8 else { throw BinaryError.malformed("EXTH record length \(length)") }
                exth.append(EXTHRecord(type: type, data: try exthReader.readData(length - 8)))
            }
            exthEnd += exthLength
        }

        let nameOffset = Int(reader.peekU32be(at: 84) ?? 0)
        let nameLength = Int(reader.peekU32be(at: 88) ?? 0)
        if nameOffset > 0, nameOffset + nameLength <= bytes.count {
            fullName = Data(bytes[nameOffset ..< nameOffset + nameLength])
            trailingPadding = max(0, bytes.count - (nameOffset + nameLength))
        } else {
            fullName = Data()
            trailingPadding = max(0, bytes.count - exthEnd)
        }
    }

    private func field(at offset: Int) -> UInt32 {
        guard offset + 4 <= headerBytes.count else { return 0 }
        return headerBytes[offset ..< offset + 4].reduce(0) { ($0 << 8) | UInt32($1) }
    }

    private mutating func setField(_ value: UInt32, at offset: Int) {
        guard offset + 4 <= headerBytes.count else { return }
        for i in 0 ..< 4 { headerBytes[offset + i] = UInt8((value >> (8 * (3 - i))) & 0xFF) }
    }

    // MARK: EXTH accessors

    func strings(_ type: EXTHType) -> [String] {
        exth.filter { $0.type == type.rawValue }.compactMap { decode($0.data) }.filter { !$0.isEmpty }
    }

    func string(_ type: EXTHType) -> String? { strings(type).first }

    func integer(_ type: EXTHType) -> UInt32? {
        guard let record = exth.first(where: { $0.type == type.rawValue }), record.data.count == 4 else { return nil }
        return record.data.reduce(0) { ($0 << 8) | UInt32($1) }
    }

    mutating func setStrings(_ values: [String], for type: EXTHType) {
        exth.removeAll { $0.type == type.rawValue }
        for value in values where !value.isEmpty {
            exth.append(EXTHRecord(type: type.rawValue, data: encode(value)))
        }
    }

    mutating func setString(_ value: String, for type: EXTHType) {
        setStrings(value.isEmpty ? [] : [value], for: type)
    }

    var title: String {
        string(.updatedTitle) ?? decode(fullName) ?? ""
    }

    mutating func setTitle(_ title: String) {
        fullName = encode(title)
        setString(title, for: .updatedTitle)
    }

    func decode(_ data: Data) -> String? {
        let text = String(data: data, encoding: stringEncoding) ?? String(data: data, encoding: .utf8)
        return text?.trimmingCharacters(in: .whitespacesAndNewlines.union(.controlCharacters))
    }

    func encode(_ string: String) -> Data {
        string.data(using: stringEncoding, allowLossyConversion: true) ?? Data(string.utf8)
    }

    var metadata: BookMetadata {
        var meta = BookMetadata()
        meta.title = title
        meta.authors = strings(.author)
        meta.publisher = string(.publisher) ?? ""
        meta.summary = string(.description).map(HTMLText.plain) ?? ""
        meta.language = string(.language) ?? ""
        meta.publishedDate = string(.publishingDate) ?? ""
        meta.subjects = strings(.subject)
        meta.isbn = string(.isbn) ?? ""
        meta.asin = string(.asin) ?? string(.asin2) ?? ""
        meta.cdeType = string(.cdeType) ?? ""
        meta.textLength = textLength
        meta.isDRMProtected = isEncrypted
        return meta
    }

    mutating func apply(_ changes: MetadataChanges) {
        if let title = changes.title { setTitle(title) }
        if let authors = changes.authors { setStrings(authors, for: .author) }
        if let publisher = changes.publisher { setString(publisher, for: .publisher) }
        if let summary = changes.summary { setString(summary, for: .description) }
        if let language = changes.language { setString(language, for: .language) }
        if let date = changes.publishedDate { setString(date, for: .publishingDate) }
        if let subjects = changes.subjects { setStrings(subjects, for: .subject) }
    }

    /// Rebuilds record 0, recomputing the EXTH block and full-name offsets.
    func serialized() -> Data {
        var header = self
        var exthWriter = ByteWriter()
        if !exth.isEmpty {
            var body = ByteWriter()
            for record in exth {
                body.u32be(record.type)
                body.u32be(UInt32(record.data.count + 8))
                body.append(record.data)
            }
            exthWriter.append(Array("EXTH".utf8))
            exthWriter.u32be(UInt32(body.count + 12))
            exthWriter.u32be(UInt32(exth.count))
            exthWriter.append(body.bytes)
            exthWriter.pad(to: 4)
        }
        if header.headerBytes.count >= 132 {
            var flags = header.field(at: 128)
            flags = exth.isEmpty ? flags & ~0x40 : flags | 0x40
            header.setField(flags, at: 128)
        }
        let nameOffset = header.headerBytes.count + exthWriter.count
        header.setField(UInt32(nameOffset), at: 84)
        header.setField(UInt32(fullName.count), at: 88)

        var writer = ByteWriter()
        writer.append(header.headerBytes)
        writer.append(exthWriter.bytes)
        writer.append(fullName)
        // Kindlegen pads record 0 generously; keep at least the original padding and 4 byte alignment.
        writer.append([UInt8](repeating: 0, count: max(2, trailingPadding)))
        writer.pad(to: 4)
        return writer.data
    }
}

/// Minimal HTML → plain text used for EXTH descriptions, which are often HTML fragments.
nonisolated enum HTMLText {
    static func plain(_ html: String) -> String {
        guard html.contains("<") else { return html }
        var text = html.replacingOccurrences(of: "<br\\s*/?>|</p>", with: "\n", options: .regularExpression)
        text = text.replacingOccurrences(of: "<[^>]+>", with: "", options: .regularExpression)
        let entities = ["&amp;": "&", "&lt;": "<", "&gt;": ">", "&quot;": "\"", "&#39;": "'", "&nbsp;": " "]
        for (entity, value) in entities { text = text.replacingOccurrences(of: entity, with: value) }
        return text.trimmingCharacters(in: .whitespacesAndNewlines)
    }
}
