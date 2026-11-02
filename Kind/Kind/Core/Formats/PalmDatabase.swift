import Foundation

/// The Palm Database container that wraps MOBI / AZW / AZW3 files.
///
/// Layout: a 78 byte header, `numRecords` 8 byte record-info entries, two bytes of padding
/// and then the records themselves back to back.
nonisolated struct PalmDatabase {
    struct Record {
        var attributes: UInt8
        var uniqueID: UInt32
        var data: Data
    }

    static let headerSize = 78

    var header: [UInt8]
    var records: [Record]

    var type: String { String(decoding: header[60 ..< 64], as: UTF8.self) }
    var creator: String { String(decoding: header[64 ..< 68], as: UTF8.self) }

    init(data: Data) throws {
        var reader = ByteReader(data)
        header = try reader.read(Self.headerSize)
        try reader.seek(to: 76)
        let count = Int(try reader.u16be())
        var entries: [(offset: Int, attributes: UInt8, uid: UInt32)] = []
        for _ in 0 ..< count {
            let offset = Int(try reader.u32be())
            let attributes = try reader.u8()
            let uid = (UInt32(try reader.u8()) << 16) | (UInt32(try reader.u8()) << 8) | UInt32(try reader.u8())
            entries.append((offset, attributes, uid))
        }
        records = []
        records.reserveCapacity(count)
        for (index, entry) in entries.enumerated() {
            let end = index + 1 < entries.count ? entries[index + 1].offset : data.count
            guard entry.offset <= end, end <= data.count else {
                throw BinaryError.malformed("record \(index) spans \(entry.offset)..<\(end)")
            }
            records.append(Record(attributes: entry.attributes, uniqueID: entry.uid,
                                  data: data.subdata(in: entry.offset ..< end)))
        }
    }

    func serialized() -> Data {
        var writer = ByteWriter(capacity: records.reduce(0) { $0 + $1.data.count } + 1024)
        var headerBytes = header
        headerBytes[76] = UInt8((records.count >> 8) & 0xFF)
        headerBytes[77] = UInt8(records.count & 0xFF)
        writer.append(headerBytes)
        var offset = Self.headerSize + records.count * 8 + 2
        for record in records {
            writer.u32be(UInt32(offset))
            writer.u8(record.attributes)
            writer.u8(UInt8((record.uniqueID >> 16) & 0xFF))
            writer.u8(UInt8((record.uniqueID >> 8) & 0xFF))
            writer.u8(UInt8(record.uniqueID & 0xFF))
            offset += record.data.count
        }
        writer.u16be(0)
        for record in records { writer.append(record.data) }
        return writer.data
    }
}
