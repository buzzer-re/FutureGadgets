import Foundation

/// Decoder for KRDS ("Kindle Reader Data Store"), the serialization Kindle firmware uses for
/// the sidecar files inside `<Book>.sdr/` (`.azw3f`, `.azw3r`, `.yjr`, `.yjf`, `.pdt`, …).
///
/// The stream is a signature followed by typed values. Each value starts with a one-byte type
/// tag; named fields are bracketed by `fieldBegin` (tag −2, followed by an untyped UTF name)
/// and `fieldEnd` (tag −1) and may nest.
nonisolated enum KRDS {
    static let signature: [UInt8] = [0x00, 0x00, 0x00, 0x00, 0x00, 0x1A, 0xB1, 0x26]

    indirect enum Value: Hashable, Sendable {
        case bool(Bool)
        case int(Int32)
        case long(Int64)
        case string(String?)
        case double(Double)
        case short(Int16)
        case float(Float)
        case byte(Int8)
        case char(UInt16)
        case field(name: String, values: [Value])

        var integer: Int64? {
            switch self {
            case let .int(v): Int64(v)
            case let .long(v): v
            case let .short(v): Int64(v)
            case let .byte(v): Int64(v)
            default: nil
            }
        }

        var number: Double? {
            switch self {
            case let .double(v): v
            case let .float(v): Double(v)
            default: integer.map(Double.init)
            }
        }

        var text: String? {
            if case let .string(s) = self { return s }
            return nil
        }

        var fieldName: String? {
            if case let .field(name, _) = self { return name }
            return nil
        }

        var children: [Value] {
            if case let .field(_, values) = self { return values }
            return []
        }
    }

    struct Document: Sendable {
        var values: [Value]

        /// Depth-first search for every field with the given name.
        func fields(named name: String) -> [Value] {
            var result: [Value] = []
            func visit(_ values: [Value]) {
                for value in values {
                    if case let .field(fieldName, children) = value {
                        if fieldName == name { result.append(value) }
                        visit(children)
                    }
                }
            }
            visit(values)
            return result
        }

        func field(named name: String) -> Value? { fields(named: name).first }

        /// Counts nested fields whose name satisfies the predicate (e.g. highlights).
        func countFields(where predicate: (String) -> Bool) -> Int {
            var count = 0
            func visit(_ values: [Value]) {
                for value in values {
                    if case let .field(name, children) = value {
                        if predicate(name) { count += 1 }
                        visit(children)
                    }
                }
            }
            visit(values)
            return count
        }
    }

    enum Tag: Int8 {
        case bool = 0, int = 1, long = 2, utf = 3, double = 4, short = 5, float = 6, byte = 7, char = 9
        case fieldBegin = -2
        case fieldEnd = -1
    }

    static func isKRDS(_ data: Data) -> Bool {
        data.count >= signature.count && [UInt8](data.prefix(signature.count)) == signature
    }

    /// Decodes a KRDS file. Decoding is tolerant: if the stream becomes unreadable part way
    /// through, everything decoded up to that point is still returned.
    static func decode(_ data: Data) throws -> Document {
        guard isKRDS(data) else { throw BinaryError.invalidMagic("KRDS") }
        var reader = ByteReader(data, offset: signature.count)
        var values: [Value] = []
        while !reader.isAtEnd {
            guard let value = try? readValue(&reader, depth: 0) else { break }
            values.append(value)
        }
        return Document(values: values)
    }

    private static func readUTF(_ reader: inout ByteReader) throws -> String? {
        let isEmpty = try reader.u8()
        if isEmpty == 1 { return nil }
        let length = Int(try reader.u16be())
        return String(decoding: try reader.read(length), as: UTF8.self)
    }

    private static func readValue(_ reader: inout ByteReader, depth: Int) throws -> Value {
        guard depth < 64 else { throw BinaryError.malformed("KRDS nesting too deep") }
        let raw = Int8(bitPattern: try reader.u8())
        guard let tag = Tag(rawValue: raw) else { throw BinaryError.malformed("KRDS tag \(raw)") }
        switch tag {
        case .bool: return .bool(try reader.u8() != 0)
        case .int: return .int(Int32(bitPattern: try reader.u32be()))
        case .long: return .long(Int64(bitPattern: try reader.u64be()))
        case .utf: return .string(try readUTF(&reader))
        case .double: return .double(Double(bitPattern: try reader.u64be()))
        case .short: return .short(Int16(bitPattern: try reader.u16be()))
        case .float: return .float(Float(bitPattern: try reader.u32be()))
        case .byte: return .byte(Int8(bitPattern: try reader.u8()))
        case .char: return .char(try reader.u16be())
        case .fieldBegin:
            let name = try readUTF(&reader) ?? ""
            var children: [Value] = []
            while true {
                guard !reader.isAtEnd else { break }
                if Int8(bitPattern: reader.bytes[reader.offset]) == Tag.fieldEnd.rawValue {
                    try reader.skip(1)
                    break
                }
                children.append(try readValue(&reader, depth: depth + 1))
            }
            return .field(name: name, values: children)
        case .fieldEnd:
            throw BinaryError.malformed("unbalanced KRDS field end")
        }
    }

    // MARK: Encoding (used by tests and for round-tripping)

    static func encode(_ values: [Value]) -> Data {
        var writer = ByteWriter()
        writer.append(signature)
        for value in values { write(value, into: &writer) }
        return writer.data
    }

    private static func writeUTF(_ string: String?, into writer: inout ByteWriter) {
        guard let string else { writer.u8(1); return }
        let bytes = Array(string.utf8)
        writer.u8(0)
        writer.u16be(UInt16(bytes.count))
        writer.append(bytes)
    }

    private static func write(_ value: Value, into writer: inout ByteWriter) {
        func tag(_ t: Tag) { writer.u8(UInt8(bitPattern: t.rawValue)) }
        switch value {
        case let .bool(v): tag(.bool); writer.u8(v ? 1 : 0)
        case let .int(v): tag(.int); writer.u32be(UInt32(bitPattern: v))
        case let .long(v): tag(.long); writer.u64be(UInt64(bitPattern: v))
        case let .string(v): tag(.utf); writeUTF(v, into: &writer)
        case let .double(v): tag(.double); writer.u64be(v.bitPattern)
        case let .short(v): tag(.short); writer.u16be(UInt16(bitPattern: v))
        case let .float(v): tag(.float); writer.u32be(v.bitPattern)
        case let .byte(v): tag(.byte); writer.u8(UInt8(bitPattern: v))
        case let .char(v): tag(.char); writer.u16be(v)
        case let .field(name, children):
            tag(.fieldBegin)
            writeUTF(name, into: &writer)
            for child in children { write(child, into: &writer) }
            tag(.fieldEnd)
        }
    }
}
