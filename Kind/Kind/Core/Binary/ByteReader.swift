import Foundation

/// Errors raised while decoding binary formats (MOBI, KRDS, MTP datasets…).
nonisolated enum BinaryError: Error, Equatable, CustomStringConvertible {
    case unexpectedEnd(offset: Int, wanted: Int)
    case invalidMagic(String)
    case malformed(String)

    var description: String {
        switch self {
        case let .unexpectedEnd(offset, wanted): "Unexpected end of data at \(offset) (wanted \(wanted) bytes)"
        case let .invalidMagic(what): "Invalid signature: \(what)"
        case let .malformed(what): "Malformed data: \(what)"
        }
    }
}

/// A cursor over a byte buffer with explicit-endianness integer reads.
nonisolated struct ByteReader {
    let bytes: [UInt8]
    private(set) var offset: Int

    init(_ data: Data, offset: Int = 0) {
        self.bytes = [UInt8](data)
        self.offset = offset
    }

    init(bytes: [UInt8], offset: Int = 0) {
        self.bytes = bytes
        self.offset = offset
    }

    var remaining: Int { bytes.count - offset }
    var isAtEnd: Bool { offset >= bytes.count }

    mutating func seek(to position: Int) throws {
        guard position >= 0, position <= bytes.count else {
            throw BinaryError.unexpectedEnd(offset: position, wanted: 0)
        }
        offset = position
    }

    mutating func skip(_ count: Int) throws {
        try seek(to: offset + count)
    }

    private func ensure(_ count: Int) throws {
        guard count >= 0, offset + count <= bytes.count else {
            throw BinaryError.unexpectedEnd(offset: offset, wanted: count)
        }
    }

    mutating func read(_ count: Int) throws -> [UInt8] {
        try ensure(count)
        defer { offset += count }
        return Array(bytes[offset ..< offset + count])
    }

    mutating func readData(_ count: Int) throws -> Data {
        Data(try read(count))
    }

    mutating func u8() throws -> UInt8 {
        try ensure(1)
        defer { offset += 1 }
        return bytes[offset]
    }

    mutating func u16be() throws -> UInt16 { UInt16(try uint(2, bigEndian: true)) }
    mutating func u32be() throws -> UInt32 { UInt32(try uint(4, bigEndian: true)) }
    mutating func u64be() throws -> UInt64 { try uint(8, bigEndian: true) }
    mutating func u16le() throws -> UInt16 { UInt16(try uint(2, bigEndian: false)) }
    mutating func u32le() throws -> UInt32 { UInt32(try uint(4, bigEndian: false)) }
    mutating func u64le() throws -> UInt64 { try uint(8, bigEndian: false) }

    private mutating func uint(_ width: Int, bigEndian: Bool) throws -> UInt64 {
        try ensure(width)
        var value: UInt64 = 0
        for i in 0 ..< width {
            let byte = UInt64(bytes[offset + (bigEndian ? i : width - 1 - i)])
            value = (value << 8) | byte
        }
        offset += width
        return value
    }

    /// Peeks at a big-endian UInt32 at an absolute position without moving the cursor.
    func peekU32be(at position: Int) -> UInt32? {
        guard position >= 0, position + 4 <= bytes.count else { return nil }
        return bytes[position ..< position + 4].reduce(0) { ($0 << 8) | UInt32($1) }
    }
}

/// Growable little/big-endian byte writer.
nonisolated struct ByteWriter {
    private(set) var bytes: [UInt8] = []

    init(capacity: Int = 0) { bytes.reserveCapacity(capacity) }

    var data: Data { Data(bytes) }
    var count: Int { bytes.count }

    mutating func append(_ other: [UInt8]) { bytes.append(contentsOf: other) }
    mutating func append(_ other: Data) { bytes.append(contentsOf: other) }
    mutating func u8(_ v: UInt8) { bytes.append(v) }
    mutating func u16be(_ v: UInt16) { uint(UInt64(v), 2, bigEndian: true) }
    mutating func u32be(_ v: UInt32) { uint(UInt64(v), 4, bigEndian: true) }
    mutating func u64be(_ v: UInt64) { uint(v, 8, bigEndian: true) }
    mutating func u16le(_ v: UInt16) { uint(UInt64(v), 2, bigEndian: false) }
    mutating func u32le(_ v: UInt32) { uint(UInt64(v), 4, bigEndian: false) }
    mutating func u64le(_ v: UInt64) { uint(v, 8, bigEndian: false) }
    mutating func pad(to alignment: Int) {
        while bytes.count % alignment != 0 { bytes.append(0) }
    }

    /// Overwrites a big-endian UInt32 at an absolute position.
    mutating func patchU32be(_ v: UInt32, at position: Int) {
        for i in 0 ..< 4 { bytes[position + i] = UInt8((v >> (8 * (3 - i))) & 0xFF) }
    }

    private mutating func uint(_ v: UInt64, _ width: Int, bigEndian: Bool) {
        for i in 0 ..< width {
            let shift = bigEndian ? 8 * (width - 1 - i) : 8 * i
            bytes.append(UInt8((v >> UInt64(shift)) & 0xFF))
        }
    }
}
