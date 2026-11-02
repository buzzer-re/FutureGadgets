import Foundation
@testable import Kind

/// Builds small but structurally valid books in memory.
enum Fixtures {
    static let jpeg = Data([0xFF, 0xD8, 0xFF, 0xE0] + Array(repeating: 0x42, count: 60) + [0xFF, 0xD9])

    static func mobi(title: String = "Original Title", authors: [String] = ["Ann Author"],
                     textLength: UInt32 = 10_000, withCover: Bool = true, encrypted: Bool = false) -> Data {
        var exth: [(UInt32, Data)] = authors.map { (100, Data($0.utf8)) }
        exth.append((103, Data("<p>A <b>fine</b> book.</p>".utf8)))
        exth.append((113, Data("B00TESTASN".utf8)))
        if withCover { exth.append((201, Data([0, 0, 0, 0]))) }

        var header = ByteWriter()
        // PalmDOC header
        header.u16be(1); header.u16be(0); header.u32be(textLength); header.u16be(1); header.u16be(4096)
        header.u16be(encrypted ? 2 : 0); header.u16be(0)
        // MOBI header (232 bytes)
        var mobi = [UInt8](repeating: 0, count: 232)
        func put(_ value: UInt32, _ offset: Int) {
            for i in 0 ..< 4 { mobi[offset - 16 + i] = UInt8((value >> (8 * (3 - UInt32(i)))) & 0xFF) }
        }
        mobi.replaceSubrange(0 ..< 4, with: Array("MOBI".utf8))
        put(232, 20)
        put(2, 24)
        put(65001, 28)
        put(2, 108) // first image record
        put(0x40, 128)
        put(0xFFFF_FFFF, 168)
        header.append(mobi)

        var exthWriter = ByteWriter()
        var body = ByteWriter()
        for (type, data) in exth { body.u32be(type); body.u32be(UInt32(data.count + 8)); body.append(data) }
        exthWriter.append(Array("EXTH".utf8)); exthWriter.u32be(UInt32(body.count + 12)); exthWriter.u32be(UInt32(exth.count))
        exthWriter.append(body.bytes); exthWriter.pad(to: 4)

        var record0 = ByteWriter()
        record0.append(header.bytes)
        let nameOffset = record0.count + exthWriter.count
        record0.append(exthWriter.bytes)
        let name = Data(title.utf8)
        record0.append(name)
        record0.append([0, 0]); record0.pad(to: 4)
        var bytes = record0.bytes
        for i in 0 ..< 4 { bytes[84 + i] = UInt8((UInt32(nameOffset) >> (8 * (3 - UInt32(i)))) & 0xFF) }
        for i in 0 ..< 4 { bytes[88 + i] = UInt8((UInt32(name.count) >> (8 * (3 - UInt32(i)))) & 0xFF) }

        let records = [Data(bytes), Data(repeating: 0x41, count: 128), jpeg]
        var pdb = ByteWriter()
        var nameField = Array("Original_Title".utf8); nameField += [UInt8](repeating: 0, count: 32 - nameField.count)
        pdb.append(nameField)
        pdb.append([UInt8](repeating: 0, count: 28))
        pdb.append(Array("BOOKMOBI".utf8))
        pdb.u32be(0); pdb.u32be(0)
        pdb.u16be(UInt16(records.count))
        var offset = 78 + records.count * 8 + 2
        for (index, record) in records.enumerated() {
            pdb.u32be(UInt32(offset)); pdb.u8(0); pdb.u8(0); pdb.u8(0); pdb.u8(UInt8(index * 2))
            offset += record.count
        }
        pdb.u16be(0)
        for record in records { pdb.append(record) }
        return pdb.data
    }
}
