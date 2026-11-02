import Foundation

// MTP (Media Transfer Protocol) is PTP (ISO 15740) plus Microsoft's extensions. All
// multi-byte values are little-endian. Every exchange is a *transaction*: a command
// container on the bulk-OUT pipe, an optional data phase in either direction, and a
// response container on the bulk-IN pipe.

nonisolated enum MTPContainerType: UInt16 {
    case command = 1, data = 2, response = 3, event = 4
}

nonisolated enum MTPOperation: UInt16 {
    case getDeviceInfo = 0x1001
    case openSession = 0x1002
    case closeSession = 0x1003
    case getStorageIDs = 0x1004
    case getStorageInfo = 0x1005
    case getNumObjects = 0x1006
    case getObjectHandles = 0x1007
    case getObjectInfo = 0x1008
    case getObject = 0x1009
    case deleteObject = 0x100B
    case sendObjectInfo = 0x100C
    case sendObject = 0x100D
    case getDevicePropValue = 0x1015
    case getPartialObject = 0x101B
    case getObjectPropValue = 0x9803
    case getPartialObject64 = 0x95C1
}

nonisolated enum MTPResponseCode: UInt16 {
    case ok = 0x2001
    case generalError = 0x2002
    case sessionNotOpen = 0x2003
    case invalidTransactionID = 0x2004
    case operationNotSupported = 0x2005
    case parameterNotSupported = 0x2006
    case incompleteTransfer = 0x2007
    case invalidStorageID = 0x2008
    case invalidObjectHandle = 0x2009
    case storeFull = 0x200C
    case objectWriteProtected = 0x200D
    case storeReadOnly = 0x200E
    case accessDenied = 0x200F
    case deviceBusy = 0x2019
    case invalidParentObject = 0x201A
    case invalidParameter = 0x201D
    case sessionAlreadyOpen = 0x201E
    case transactionCancelled = 0x201F
}

nonisolated enum MTPObjectFormat: UInt16 {
    case undefined = 0x3000
    case association = 0x3001
    case text = 0x3004
}

nonisolated enum MTPDeviceProperty: UInt16 {
    case batteryLevel = 0x5001
    case friendlyName = 0xD402
}

nonisolated enum MTPError: LocalizedError, Equatable {
    case response(UInt16, operation: UInt16)
    case unexpectedContainer(type: UInt16, code: UInt16)
    case transactionMismatch(expected: UInt32, got: UInt32)
    case shortRead
    case noInterface
    case notFound(String)

    var errorDescription: String? {
        switch self {
        case let .response(code, op):
            let name = MTPResponseCode(rawValue: code).map { "\($0)" } ?? String(format: "0x%04X", code)
            return "The Kindle rejected the request (\(name), op 0x\(String(op, radix: 16)))."
        case let .unexpectedContainer(type, code): return "Unexpected MTP container \(type)/0x\(String(code, radix: 16))."
        case let .transactionMismatch(expected, got): return "MTP transaction mismatch (expected \(expected), got \(got))."
        case .shortRead: return "The Kindle stopped responding mid-transfer."
        case .noInterface: return "No MTP interface found on the Kindle."
        case let .notFound(path): return "“\(path)” was not found on the Kindle."
        }
    }
}

/// A command or response container.
nonisolated struct MTPContainer: Equatable {
    static let headerSize = 12

    var type: MTPContainerType
    var code: UInt16
    var transactionID: UInt32
    var parameters: [UInt32] = []

    func encoded() -> Data {
        var writer = ByteWriter(capacity: Self.headerSize + parameters.count * 4)
        writer.u32le(UInt32(Self.headerSize + parameters.count * 4))
        writer.u16le(type.rawValue)
        writer.u16le(code)
        writer.u32le(transactionID)
        for parameter in parameters { writer.u32le(parameter) }
        return writer.data
    }

    static func dataHeader(payloadLength: Int, code: UInt16, transactionID: UInt32) -> Data {
        var writer = ByteWriter(capacity: headerSize)
        let length = headerSize + payloadLength
        writer.u32le(length > Int(UInt32.max) ? UInt32.max : UInt32(length))
        writer.u16le(MTPContainerType.data.rawValue)
        writer.u16le(code)
        writer.u32le(transactionID)
        return writer.data
    }

    struct Header {
        var length: UInt32
        var type: UInt16
        var code: UInt16
        var transactionID: UInt32
    }

    static func parseHeader(_ data: Data) throws -> Header {
        var reader = ByteReader(data)
        return Header(length: try reader.u32le(), type: try reader.u16le(),
                      code: try reader.u16le(), transactionID: try reader.u32le())
    }

    static func decodeResponse(_ data: Data) throws -> MTPContainer {
        let header = try parseHeader(data)
        guard let type = MTPContainerType(rawValue: header.type) else {
            throw MTPError.unexpectedContainer(type: header.type, code: header.code)
        }
        var reader = ByteReader(data, offset: headerSize)
        var parameters: [UInt32] = []
        let end = min(Int(header.length), data.count)
        while reader.offset + 4 <= end { parameters.append(try reader.u32le()) }
        return MTPContainer(type: type, code: header.code, transactionID: header.transactionID, parameters: parameters)
    }
}

// MARK: - Datasets

nonisolated extension ByteReader {
    /// PTP string: UInt8 character count (including the terminating NUL), then UTF-16LE.
    mutating func mtpString() throws -> String {
        let count = Int(try u8())
        guard count > 0 else { return "" }
        var units: [UInt16] = []
        units.reserveCapacity(count)
        for _ in 0 ..< count { units.append(try u16le()) }
        if units.last == 0 { units.removeLast() }
        return String(decoding: units, as: UTF16.self)
    }

    mutating func mtpArray16() throws -> [UInt16] {
        let count = Int(try u32le())
        return try (0 ..< count).map { _ in try u16le() }
    }

    mutating func mtpArray32() throws -> [UInt32] {
        let count = Int(try u32le())
        return try (0 ..< count).map { _ in try u32le() }
    }
}

nonisolated extension ByteWriter {
    mutating func mtpString(_ string: String) {
        guard !string.isEmpty else { u8(0); return }
        let units = Array(string.utf16.prefix(254)) + [0]
        u8(UInt8(units.count))
        for unit in units { u16le(unit) }
    }
}

nonisolated struct MTPDeviceInfo: Sendable, Equatable {
    var standardVersion: UInt16
    var vendorExtensionID: UInt32
    var vendorExtensionDescription: String
    var operationsSupported: Set<UInt16>
    var devicePropertiesSupported: Set<UInt16>
    var manufacturer: String
    var model: String
    var deviceVersion: String
    var serialNumber: String

    init(data: Data) throws {
        var r = ByteReader(data)
        standardVersion = try r.u16le()
        vendorExtensionID = try r.u32le()
        _ = try r.u16le() // vendor extension version
        vendorExtensionDescription = try r.mtpString()
        _ = try r.u16le() // functional mode
        operationsSupported = Set(try r.mtpArray16())
        _ = try r.mtpArray16() // events
        devicePropertiesSupported = Set(try r.mtpArray16())
        _ = try r.mtpArray16() // capture formats
        _ = try r.mtpArray16() // playback formats
        manufacturer = try r.mtpString()
        model = try r.mtpString()
        deviceVersion = try r.mtpString()
        serialNumber = try r.mtpString()
    }

    func supports(_ operation: MTPOperation) -> Bool { operationsSupported.contains(operation.rawValue) }
}

nonisolated struct MTPStorageInfo: Sendable, Equatable {
    var storageType: UInt16
    var filesystemType: UInt16
    var accessCapability: UInt16
    var maxCapacity: UInt64
    var freeSpace: UInt64
    var description: String
    var volumeIdentifier: String

    init(data: Data) throws {
        var r = ByteReader(data)
        storageType = try r.u16le()
        filesystemType = try r.u16le()
        accessCapability = try r.u16le()
        maxCapacity = try r.u64le()
        freeSpace = try r.u64le()
        _ = try r.u32le() // free space in objects
        description = try r.mtpString()
        volumeIdentifier = (try? r.mtpString()) ?? ""
    }
}

nonisolated struct MTPObjectInfo: Sendable, Equatable {
    var storageID: UInt32
    var format: UInt16
    var protectionStatus: UInt16 = 0
    var compressedSize: UInt32
    var parent: UInt32
    var associationType: UInt16 = 0
    var filename: String
    var dateCreated: String = ""
    var dateModified: String = ""

    var isFolder: Bool { format == MTPObjectFormat.association.rawValue }

    init(storageID: UInt32, format: UInt16, compressedSize: UInt32, parent: UInt32, associationType: UInt16 = 0,
         filename: String, dateModified: String = "") {
        self.storageID = storageID
        self.format = format
        self.compressedSize = compressedSize
        self.parent = parent
        self.associationType = associationType
        self.filename = filename
        self.dateModified = dateModified
        self.dateCreated = dateModified
    }

    init(data: Data) throws {
        var r = ByteReader(data)
        storageID = try r.u32le()
        format = try r.u16le()
        protectionStatus = try r.u16le()
        compressedSize = try r.u32le()
        try r.skip(2 + 4 * 6) // thumb format, thumb size, thumb w/h, image w/h, bit depth
        parent = try r.u32le()
        associationType = try r.u16le()
        _ = try r.u32le() // association description
        _ = try r.u32le() // sequence number
        filename = try r.mtpString()
        dateCreated = (try? r.mtpString()) ?? ""
        dateModified = (try? r.mtpString()) ?? ""
    }

    func encoded() -> Data {
        var w = ByteWriter(capacity: 128)
        w.u32le(storageID)
        w.u16le(format)
        w.u16le(protectionStatus)
        w.u32le(compressedSize)
        w.u16le(0) // thumb format
        for _ in 0 ..< 6 { w.u32le(0) } // thumb size, thumb w/h, image w/h, bit depth
        w.u32le(parent)
        w.u16le(associationType)
        w.u32le(0) // association description
        w.u32le(0) // sequence number
        w.mtpString(filename)
        w.mtpString(dateCreated)
        w.mtpString(dateModified)
        w.mtpString("") // keywords
        return w.data
    }
}

/// PTP timestamps look like `20240131T235959` optionally followed by `.s`, `Z` or `±hhmm`.
nonisolated enum MTPDate {
    static func parse(_ string: String) -> Date? {
        let digits = string.prefix(15)
        guard digits.count == 15 else { return nil }
        var components = DateComponents()
        func int(_ from: Int, _ length: Int) -> Int? {
            let start = digits.index(digits.startIndex, offsetBy: from)
            return Int(digits[start ..< digits.index(start, offsetBy: length)])
        }
        components.year = int(0, 4)
        components.month = int(4, 2)
        components.day = int(6, 2)
        components.hour = int(9, 2)
        components.minute = int(11, 2)
        components.second = int(13, 2)
        components.timeZone = string.hasSuffix("Z") ? TimeZone(identifier: "UTC") : .current
        return Calendar(identifier: .gregorian).date(from: components)
    }

    static func format(_ date: Date) -> String {
        let c = Calendar(identifier: .gregorian).dateComponents(in: .current, from: date)
        return String(format: "%04d%02d%02dT%02d%02d%02d", c.year ?? 0, c.month ?? 0, c.day ?? 0,
                      c.hour ?? 0, c.minute ?? 0, c.second ?? 0)
    }
}
