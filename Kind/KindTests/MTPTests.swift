import Foundation
import Testing
@testable import Kind

/// An in-memory MTP responder speaking the same container protocol as a Kindle.
/// It deliberately returns data in small packets so the session's reassembly and
/// zero-length-packet handling are exercised.
final class FakeMTPDevice: MTPTransport, @unchecked Sendable {
    struct Object {
        var name: String
        var parent: UInt32
        var data: Data
        var isFolder: Bool
    }

    let maxPacketSize = 64
    let storageID: UInt32 = 0x0001_0001
    var objects: [UInt32: Object] = [:]
    var nextHandle: UInt32 = 100
    var supportsPartial = true
    var sessionOpen = false

    var outbox: [Data] = []
    var drainClearsQueue = true
    private var pendingCommand: MTPContainer?
    private var incomingData = Data()
    private var expectedDataLength = 0
    private var pendingObjectInfo: MTPObjectInfo?
    private var pendingParent: UInt32 = 0
    private(set) var zeroLengthPacketsReceived = 0

    init() {
        objects[1] = Object(name: "documents", parent: 0, data: Data(), isFolder: true)
        objects[2] = Object(name: "system", parent: 0, data: Data(), isFolder: true)
        objects[3] = Object(name: "Dune.azw3", parent: 1, data: Fixtures.mobi(title: "Dune"), isFolder: false)
        objects[4] = Object(name: "Dune.sdr", parent: 1, data: Data(), isFolder: true)
        objects[5] = Object(name: "Dune.azw3f", parent: 4,
                            data: KRDS.encode([.field(name: "book.info.store", values: [.double(64)])]), isFolder: false)
    }

    // MARK: MTPTransport

    func write(_ data: Data, timeout: TimeInterval) throws {
        if data.isEmpty { zeroLengthPacketsReceived += 1; return }
        if expectedDataLength > 0 {
            incomingData.append(data)
            if incomingData.count >= expectedDataLength { finishDataPhase() }
            return
        }
        let header = try MTPContainer.parseHeader(data)
        if header.type == MTPContainerType.data.rawValue {
            expectedDataLength = Int(header.length)
            incomingData = data
            if incomingData.count >= expectedDataLength { finishDataPhase() }
            return
        }
        let command = try MTPContainer.decodeResponse(data)
        pendingCommand = command
        handle(command)
    }

    func read(maxLength: Int, timeout: TimeInterval) throws -> Data {
        guard !outbox.isEmpty else { throw MTPError.shortRead }
        var packet = outbox.removeFirst()
        if packet.count > maxLength {
            outbox.insert(packet.subdata(in: maxLength ..< packet.count), at: 0)
            packet = packet.subdata(in: 0 ..< maxLength)
        }
        return packet
    }

    func recover() {}
    func close() {}
    func drain() { if drainClearsQueue { outbox.removeAll() } }

    /// Simulates packets a previous process left queued on the device.
    func injectStaleTransaction(_ tid: UInt32) {
        var data = MTPContainer.dataHeader(payloadLength: 4, code: 0x1007, transactionID: tid)
        data.append(Data([1, 0, 0, 0]))
        outbox.append(data)
        outbox.append(MTPContainer(type: .response, code: MTPResponseCode.ok.rawValue, transactionID: tid).encoded())
    }

    // MARK: Device behaviour

    private func respond(_ code: MTPResponseCode, _ tid: UInt32, _ params: [UInt32] = []) {
        outbox.append(MTPContainer(type: .response, code: code.rawValue, transactionID: tid, parameters: params).encoded())
    }

    private func sendData(_ payload: Data, _ command: MTPContainer) {
        var packet = MTPContainer.dataHeader(payloadLength: payload.count, code: command.code, transactionID: command.transactionID)
        packet.append(payload)
        // Split into packets of 3×maxPacketSize to force multiple reads.
        var offset = 0
        while offset < packet.count {
            let end = min(packet.count, offset + maxPacketSize * 3)
            outbox.append(packet.subdata(in: offset ..< end))
            offset = end
        }
        if packet.count % maxPacketSize == 0 { outbox.append(Data()) }
        respond(.ok, command.transactionID)
    }

    private func handle(_ command: MTPContainer) {
        let tid = command.transactionID
        let p = command.parameters
        guard let op = MTPOperation(rawValue: command.code) else { return respond(.operationNotSupported, tid) }
        if op != .getDeviceInfo && op != .openSession && !sessionOpen { return respond(.sessionNotOpen, tid) }
        switch op {
        case .getDeviceInfo:
            sendData(deviceInfo(), command)
        case .openSession:
            if sessionOpen { return respond(.sessionAlreadyOpen, tid) }
            sessionOpen = true
            respond(.ok, tid)
        case .closeSession:
            sessionOpen = false
            respond(.ok, tid)
        case .getStorageIDs:
            var w = ByteWriter(); w.u32le(1); w.u32le(storageID)
            sendData(w.data, command)
        case .getStorageInfo:
            var w = ByteWriter()
            w.u16le(3); w.u16le(2); w.u16le(0); w.u64le(8_000_000_000); w.u64le(5_000_000_000); w.u32le(0xFFFF_FFFF)
            w.mtpString("Internal Storage"); w.mtpString("")
            sendData(w.data, command)
        case .getObjectHandles:
            let parent = p[2] == 0xFFFF_FFFF ? 0 : p[2]
            let handles = objects.filter { $0.value.parent == parent }.keys.sorted()
            var w = ByteWriter(); w.u32le(UInt32(handles.count)); handles.forEach { w.u32le($0) }
            sendData(w.data, command)
        case .getObjectInfo:
            guard let object = objects[p[0]] else { return respond(.invalidObjectHandle, tid) }
            let info = MTPObjectInfo(storageID: storageID, format: object.isFolder ? 0x3001 : 0x3000,
                                     compressedSize: UInt32(object.data.count),
                                     parent: object.parent, associationType: object.isFolder ? 1 : 0,
                                     filename: object.name, dateModified: "20260101T120000")
            sendData(info.encoded(), command)
        case .getObject:
            guard let object = objects[p[0]] else { return respond(.invalidObjectHandle, tid) }
            sendData(object.data, command)
        case .getPartialObject:
            guard let object = objects[p[0]] else { return respond(.invalidObjectHandle, tid) }
            let start = min(Int(p[1]), object.data.count)
            let end = min(object.data.count, start + Int(p[2]))
            sendData(object.data.subdata(in: start ..< end), command)
        case .deleteObject:
            guard objects[p[0]] != nil else { return respond(.invalidObjectHandle, tid) }
            removeTree(p[0])
            respond(.ok, tid)
        case .sendObjectInfo:
            pendingParent = p[1]
        case .sendObject:
            break // wait for the data phase
        default:
            respond(.operationNotSupported, tid)
        }
    }

    private func removeTree(_ handle: UInt32) {
        for child in objects.filter({ $0.value.parent == handle }).keys { removeTree(child) }
        objects[handle] = nil
    }

    private func finishDataPhase() {
        let payload = incomingData.subdata(in: MTPContainer.headerSize ..< incomingData.count)
        expectedDataLength = 0
        incomingData = Data()
        guard let command = pendingCommand else { return }
        switch MTPOperation(rawValue: command.code) {
        case .sendObjectInfo:
            guard let info = try? MTPObjectInfo(data: payload) else { return respond(.invalidParameter, command.transactionID) }
            pendingObjectInfo = info
            let handle = nextHandle
            nextHandle += 1
            let parent = pendingParent == 0xFFFF_FFFF ? 0 : pendingParent
            objects[handle] = Object(name: info.filename, parent: parent, data: Data(), isFolder: info.isFolder)
            respond(.ok, command.transactionID, [storageID, pendingParent, handle])
        case .sendObject:
            objects[nextHandle - 1]?.data = payload
            respond(.ok, command.transactionID)
        default:
            respond(.generalError, command.transactionID)
        }
    }

    private func deviceInfo() -> Data {
        var w = ByteWriter()
        w.u16le(100); w.u32le(6); w.u16le(100); w.mtpString("microsoft.com: 1.0;"); w.u16le(0)
        var ops: [UInt16] = [0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1007, 0x1008, 0x1009, 0x100B, 0x100C, 0x100D]
        if supportsPartial { ops.append(0x101B) }
        w.u32le(UInt32(ops.count)); ops.forEach { w.u16le($0) }
        w.u32le(0) // events
        w.u32le(1); w.u16le(0x5001) // device properties
        w.u32le(0); w.u32le(0)
        w.mtpString("Amazon"); w.mtpString("Kindle Paperwhite"); w.mtpString("5.17.1"); w.mtpString("G8S1ABCDEF123456")
        return w.data
    }
}

@Suite("MTP driver")
struct MTPTests {
    @Test func containerEncoding() throws {
        let container = MTPContainer(type: .command, code: 0x1007, transactionID: 9, parameters: [1, 0, 0xFFFF_FFFF])
        let data = container.encoded()
        #expect(data.count == 24)
        #expect([UInt8](data.prefix(8)) == [24, 0, 0, 0, 1, 0, 0x07, 0x10])
        #expect(try MTPContainer.decodeResponse(data) == container)
    }

    @Test func stringsAreUTF16WithTerminator() throws {
        var writer = ByteWriter()
        writer.mtpString("Añ📖")
        #expect(writer.bytes.first == 5) // 4 UTF-16 units + NUL
        var reader = ByteReader(writer.data)
        #expect(try reader.mtpString() == "Añ📖")
    }

    @Test func objectInfoRoundTrips() throws {
        let info = MTPObjectInfo(storageID: 0x10001, format: 0x3000, compressedSize: 1234, parent: 7,
                                 filename: "Paper.pdf", dateModified: "20260928T101500")
        let decoded = try MTPObjectInfo(data: info.encoded())
        #expect(decoded == info)
        #expect(MTPDate.parse(decoded.dateModified) != nil)
    }

    @Test func sessionOpensAndReadsDeviceInfo() throws {
        let device = FakeMTPDevice()
        let session = MTPSession(transport: device)
        try session.open()
        #expect(session.deviceInfo?.model == "Kindle Paperwhite")
        #expect(session.supportsPartialReads)
        #expect(try session.storageIDs() == [0x0001_0001])
        #expect(try session.storageInfo(0x0001_0001).freeSpace == 5_000_000_000)
    }

    @Test func drainsPacketsLeftByAPreviousProcess() throws {
        let device = FakeMTPDevice()
        device.sessionOpen = true
        device.outbox.append(MTPContainer(type: .response, code: MTPResponseCode.sessionAlreadyOpen.rawValue, transactionID: 0).encoded())
        let session = MTPSession(transport: device)
        try session.open()
        #expect(session.deviceInfo?.model == "Kindle Paperwhite")
    }

    @Test func skipsStaleContainersMidSession() throws {
        let device = FakeMTPDevice()
        let session = MTPSession(transport: device)
        try session.open()
        device.injectStaleTransaction(900)
        #expect(try session.storageIDs() == [0x0001_0001])
        device.injectStaleTransaction(901)
        #expect(try session.storageInfo(0x0001_0001).description == "Internal Storage")
    }

    @Test func reopensWhenSessionAlreadyOpen() throws {
        let device = FakeMTPDevice()
        device.sessionOpen = true
        let session = MTPSession(transport: device)
        try session.open()
        #expect(session.isOpen)
    }

    @Test func fileSystemListsReadsUploadsAndDeletes() async throws {
        let device = FakeMTPDevice()
        let fs = try await MTPFileSystem.connect(transport: device)

        let items = try await fs.list("documents")
        #expect(Set(items.map(\.path)) == ["documents/Dune.azw3", "documents/Dune.sdr", "documents/Dune.sdr/Dune.azw3f"])

        let summary = try await MOBIBook.read(from: RemoteFileSource(fileSystem: fs, item: items.first { $0.name == "Dune.azw3" }!))
        #expect(summary.metadata.title == "Dune")
        #expect(summary.cover == Fixtures.jpeg)

        // Upload a file whose container length is an exact multiple of the packet size.
        let payload = Data((0 ..< (64 * 20 - MTPContainer.headerSize)).map { UInt8($0 & 0xFF) })
        let source = FileManager.default.temporaryDirectory.appendingPathComponent("kind-test-\(UUID()).pdf")
        try payload.write(to: source)
        defer { try? FileManager.default.removeItem(at: source) }
        let uploaded = try await fs.upload(source, to: "documents", name: "Paper.pdf", progress: nil)
        #expect(uploaded.path == "documents/Paper.pdf")
        #expect(try await fs.read("documents/Paper.pdf") == payload)
        #expect(device.zeroLengthPacketsReceived >= 1)

        // Uploading again replaces rather than duplicating.
        _ = try await fs.upload(source, to: "documents", name: "Paper.pdf", progress: nil)
        #expect(device.objects.values.filter { $0.name == "Paper.pdf" }.count == 1)

        try await fs.delete("documents/Dune.sdr")
        #expect(!device.objects.values.contains { $0.name == "Dune.azw3f" })
        #expect(try await fs.storage().capacity == 8_000_000_000)
    }

    @Test func fallsBackToWholeObjectReadsWithoutPartialSupport() async throws {
        let device = FakeMTPDevice()
        device.supportsPartial = false
        let fs = try await MTPFileSystem.connect(transport: device)
        _ = try await fs.list("documents")
        let head = try await fs.read("documents/Dune.azw3", offset: 60, length: 8)
        #expect(String(decoding: head, as: UTF8.self) == "BOOKMOBI")
    }

    @Test func scannerBuildsLibraryWithProgress() async throws {
        let device = FakeMTPDevice()
        let fs = try await MTPFileSystem.connect(transport: device)
        let scanner = LibraryScanner(fileSystem: fs, deviceID: "test-\(UUID())", cache: MetadataCache(deviceID: "test-\(UUID())"), isLocal: false)
        let listing = try await scanner.list()
        #expect(listing.books.count == 1)
        let book = try #require(listing.books.first)
        let enriched = await scanner.enrich(book, sidecars: listing.sidecarFiles[book.sidecarPath ?? ""] ?? [])
        #expect(enriched.title == "Dune")
        #expect(enriched.progress?.fraction == 0.64)
    }
}
