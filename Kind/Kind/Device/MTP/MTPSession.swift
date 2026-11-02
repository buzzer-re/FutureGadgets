import Foundation

/// The raw pipes an MTP session runs over. Implemented by `USBMTPTransport` for real
/// hardware and by an in-memory fake in the tests.
nonisolated protocol MTPTransport: AnyObject, Sendable {
    /// Max packet size of the bulk endpoints (512 for USB 2.0 high speed, 1024 for SuperSpeed).
    var maxPacketSize: Int { get }
    func write(_ data: Data, timeout: TimeInterval) throws
    /// Reads up to `maxLength` bytes; returns fewer when the device ends the transfer.
    func read(maxLength: Int, timeout: TimeInterval) throws -> Data
    /// Clears a halted endpoint after a failed transfer.
    func recover()
    /// Discards anything the device still has queued, e.g. from a session a previous
    /// process didn't close before it quit.
    func drain()
    /// Cancels in-flight transfers. Unlike the rest of the protocol this may be called from any
    /// thread, so a blocked reader can be released (e.g. when the app quits mid-scan).
    func cancelPendingIO()
    func close()
}

nonisolated extension MTPTransport {
    func drain() {}
    func cancelPendingIO() {}
}

/// Synchronous MTP session. Blocking by design; `MTPFileSystem` confines it to a dedicated
/// serial queue so it never touches the main thread or the cooperative pool. That confinement
/// is what makes the `@unchecked Sendable` conformance sound.
nonisolated final class MTPSession: @unchecked Sendable {
    enum Outgoing {
        case bytes(Data)
        case file(URL, size: Int64)
    }

    struct Response {
        var code: UInt16
        var parameters: [UInt32]
    }

    private let transport: MTPTransport
    private var transactionID: UInt32 = 0
    private let chunkSize: Int
    private(set) var isOpen = false
    private(set) var deviceInfo: MTPDeviceInfo?

    static let rootParent: UInt32 = 0xFFFF_FFFF
    static let allStorages: UInt32 = 0xFFFF_FFFF

    init(transport: MTPTransport) {
        self.transport = transport
        // Large transfers in multiples of the packet size keep USB throughput high.
        let packet = max(64, transport.maxPacketSize)
        chunkSize = (1 << 20) / packet * packet
    }

    // MARK: Session

    func open() throws {
        transport.drain()
        deviceInfo = try MTPDeviceInfo(data: transactIn(.getDeviceInfo, sessionless: true))
        transactionID = 0
        let response = try transact(.openSession, parameters: [1], sessionless: true)
        if response.code == MTPResponseCode.sessionAlreadyOpen.rawValue {
            _ = try? transact(.closeSession)
            transactionID = 0
            try check(transact(.openSession, parameters: [1], sessionless: true), .openSession)
        } else {
            try check(response, .openSession)
        }
        isOpen = true
    }

    /// Thread-safe: releases a transfer that is blocking the session's queue.
    func cancelPendingIO() {
        transport.cancelPendingIO()
    }

    func close() {
        if isOpen { _ = try? transact(.closeSession) }
        isOpen = false
        transport.close()
    }

    // MARK: Operations

    func storageIDs() throws -> [UInt32] {
        var reader = ByteReader(try transactIn(.getStorageIDs))
        return try reader.mtpArray32()
    }

    func storageInfo(_ storage: UInt32) throws -> MTPStorageInfo {
        try MTPStorageInfo(data: transactIn(.getStorageInfo, parameters: [storage]))
    }

    func objectHandles(storage: UInt32, parent: UInt32) throws -> [UInt32] {
        var reader = ByteReader(try transactIn(.getObjectHandles, parameters: [storage, 0, parent]))
        return try reader.mtpArray32()
    }

    func objectInfo(_ handle: UInt32) throws -> MTPObjectInfo {
        try MTPObjectInfo(data: transactIn(.getObjectInfo, parameters: [handle]))
    }

    func object(_ handle: UInt32, progress: ((Int64, Int64) -> Void)? = nil, sink: (Data) throws -> Void) throws {
        try withoutActuallyEscaping(sink) { sink in
            let response = try transaction(.getObject, parameters: [handle], outgoing: nil, incoming: sink, progress: progress)
            try check(response, .getObject)
        }
    }

    var supportsPartialReads: Bool { deviceInfo?.supports(.getPartialObject) ?? false }

    func partialObject(_ handle: UInt32, offset: UInt32, length: UInt32) throws -> Data {
        try transactIn(.getPartialObject, parameters: [handle, offset, length])
    }

    func deleteObject(_ handle: UInt32) throws {
        try check(transact(.deleteObject, parameters: [handle, 0]), .deleteObject)
    }

    /// Returns the handle assigned to the new object.
    func sendObject(info: MTPObjectInfo, storage: UInt32, parent: UInt32, contents: Outgoing?,
                    progress: ((Int64, Int64) -> Void)? = nil) throws -> UInt32 {
        let infoResponse = try transaction(.sendObjectInfo, parameters: [storage, parent],
                                           outgoing: .bytes(info.encoded()), incoming: nil, progress: nil)
        try check(infoResponse, .sendObjectInfo)
        guard infoResponse.parameters.count >= 3 else { throw MTPError.response(infoResponse.code, operation: MTPOperation.sendObjectInfo.rawValue) }
        let handle = infoResponse.parameters[2]
        if let contents {
            let response = try transaction(.sendObject, parameters: [], outgoing: contents, incoming: nil,
                                           progress: progress, responseTimeout: 120)
            try check(response, .sendObject)
        }
        return handle
    }

    func deviceProperty(_ property: MTPDeviceProperty) throws -> Data {
        try transactIn(.getDevicePropValue, parameters: [UInt32(property.rawValue)])
    }

    // MARK: Transactions

    private func check(_ response: Response, _ operation: MTPOperation) throws {
        guard response.code == MTPResponseCode.ok.rawValue else {
            throw MTPError.response(response.code, operation: operation.rawValue)
        }
    }

    private func transact(_ operation: MTPOperation, parameters: [UInt32] = [], sessionless: Bool = false) throws -> Response {
        try transaction(operation, parameters: parameters, outgoing: nil, incoming: nil, progress: nil, sessionless: sessionless)
    }

    private func transactIn(_ operation: MTPOperation, parameters: [UInt32] = [], sessionless: Bool = false) throws -> Data {
        var data = Data()
        let response = try transaction(operation, parameters: parameters, outgoing: nil,
                                       incoming: { data.append($0) }, progress: nil, sessionless: sessionless)
        try check(response, operation)
        return data
    }

    private func nextTransactionID(sessionless: Bool) -> UInt32 {
        // GetDeviceInfo/OpenSession run outside a session and use transaction ID 0.
        if sessionless { return 0 }
        transactionID = transactionID == 0xFFFF_FFFE ? 1 : transactionID + 1
        return transactionID
    }

    private func transaction(_ operation: MTPOperation, parameters: [UInt32], outgoing: Outgoing?,
                             incoming: ((Data) throws -> Void)?, progress: ((Int64, Int64) -> Void)?,
                             sessionless: Bool = false, responseTimeout: TimeInterval = 30) throws -> Response {
        let tid = nextTransactionID(sessionless: sessionless)
        do {
            let command = MTPContainer(type: .command, code: operation.rawValue, transactionID: tid, parameters: parameters)
            try transport.write(command.encoded(), timeout: 5)

            if let outgoing {
                try sendData(outgoing, code: operation.rawValue, transactionID: tid, progress: progress)
            }

            var pending: Data?
            if let incoming {
                pending = try receiveData(code: operation.rawValue, transactionID: tid, sink: incoming, progress: progress)
            }
            return try readResponse(transactionID: tid, prefetched: pending, timeout: responseTimeout)
        } catch {
            transport.recover()
            throw error
        }
    }

    private func sendData(_ outgoing: Outgoing, code: UInt16, transactionID: UInt32, progress: ((Int64, Int64) -> Void)?) throws {
        switch outgoing {
        case let .bytes(payload):
            var packet = MTPContainer.dataHeader(payloadLength: payload.count, code: code, transactionID: transactionID)
            packet.append(payload)
            try writeChunked(packet)
        case let .file(url, size):
            let handle = try FileHandle(forReadingFrom: url)
            defer { try? handle.close() }
            var buffer = MTPContainer.dataHeader(payloadLength: Int(size), code: code, transactionID: transactionID)
            var sent: Int64 = 0
            let total = Int64(MTPContainer.headerSize) + size
            while true {
                let room = chunkSize - buffer.count
                if room > 0, let chunk = try handle.read(upToCount: room), !chunk.isEmpty {
                    buffer.append(chunk)
                    sent += Int64(chunk.count)
                }
                if buffer.isEmpty { break }
                try transport.write(buffer, timeout: 30)
                buffer.removeAll(keepingCapacity: true)
                progress?(sent, size)
                if sent >= size { break }
            }
            if total % Int64(transport.maxPacketSize) == 0 { try transport.write(Data(), timeout: 5) }
        }
    }

    private func writeChunked(_ data: Data) throws {
        var offset = 0
        while offset < data.count {
            let end = min(data.count, offset + chunkSize)
            try transport.write(data.subdata(in: offset ..< end), timeout: 30)
            offset = end
        }
        // A transfer that is an exact multiple of the packet size must be terminated by a ZLP.
        if data.count % transport.maxPacketSize == 0 { try transport.write(Data(), timeout: 5) }
    }

    /// Reads a data phase, streaming payload chunks into `sink`. Returns a response container if
    /// the device skipped the data phase (e.g. to report an error).
    private func receiveData(code: UInt16, transactionID: UInt32, sink: (Data) throws -> Void,
                             progress: ((Int64, Int64) -> Void)?) throws -> Data? {
        var first = Data()
        var header: MTPContainer.Header?
        for _ in 0 ..< 8 {
            first = try transport.read(maxLength: chunkSize, timeout: 30)
            guard first.count >= MTPContainer.headerSize else { continue }
            let candidate = try MTPContainer.parseHeader(first)
            // Anything tagged with another transaction is left over from an earlier exchange.
            guard candidate.transactionID == transactionID else { continue }
            header = candidate
            break
        }
        guard let header else { throw MTPError.shortRead }
        if header.type == MTPContainerType.response.rawValue { return first }
        guard header.type == MTPContainerType.data.rawValue else {
            throw MTPError.unexpectedContainer(type: header.type, code: header.code)
        }

        let total = Int64(header.length) - Int64(MTPContainer.headerSize)
        let unbounded = header.length == UInt32.max
        var received = Int64(first.count - MTPContainer.headerSize)
        if received > 0 { try sink(first.subdata(in: MTPContainer.headerSize ..< first.count)) }
        progress?(received, total)

        // Keep reading until the announced length arrives (or a short packet ends an unbounded transfer).
        var lastCount = first.count
        while unbounded ? lastCount % transport.maxPacketSize == 0 && lastCount > 0 : received < total {
            let chunk = try transport.read(maxLength: chunkSize, timeout: 30)
            lastCount = chunk.count
            if chunk.isEmpty { if unbounded { break } else { throw MTPError.shortRead } }
            try sink(chunk)
            received += Int64(chunk.count)
            progress?(received, total)
        }
        return nil
    }

    private func readResponse(transactionID: UInt32, prefetched: Data?, timeout: TimeInterval) throws -> Response {
        var pending = prefetched
        // Skip zero-length packets left over from a data phase that ended on a packet boundary,
        // and stale containers from an earlier transaction the device still had queued.
        for _ in 0 ..< 8 {
            let data: Data
            if let prefetched = pending, !prefetched.isEmpty {
                data = prefetched
                pending = nil
            } else {
                data = try transport.read(maxLength: max(512, transport.maxPacketSize), timeout: timeout)
            }
            guard !data.isEmpty else { continue }
            let container = try MTPContainer.decodeResponse(data)
            guard container.type == .response, container.transactionID == transactionID else { continue }
            return Response(code: container.code, parameters: container.parameters)
        }
        throw MTPError.shortRead
    }
}
