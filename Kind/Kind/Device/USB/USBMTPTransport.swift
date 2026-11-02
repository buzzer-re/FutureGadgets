import Foundation
import IOKit
import IOKit.usb
import IOUSBHost

/// Bulk-pipe transport for an MTP interface, built directly on IOUSBHost (user-space USB).
///
/// The interface is opened with `.deviceSeize`, which asks macOS' own PTP daemon
/// (Image Capture's `ptpcamerad`) to let go of the Kindle so we can drive it ourselves.
/// All I/O is synchronous and must be called from a single queue (see `MTPFileSystem`).
nonisolated final class USBMTPTransport: MTPTransport, @unchecked Sendable {
    private let interface: IOUSBHostInterface
    private let bulkIn: IOUSBHostPipe
    private let bulkOut: IOUSBHostPipe
    let maxPacketSize: Int

    /// IOUSBHost DMA buffers have a fixed length (resizing one crashes), and a transfer moves
    /// exactly `length` bytes. So buffers are allocated at the exact size and reused by size.
    private var buffers: [Int: NSMutableData] = [:]
    private var isClosed = false

    init(interfaceService: io_service_t) throws {
        let interestQueue = DispatchQueue(label: "app.kind.usb.interest")
        interface = try IOUSBHostInterface(__ioService: interfaceService, options: [.deviceSeize],
                                           queue: interestQueue, interestHandler: nil)

        let endpoints = Self.bulkEndpoints(of: interface)
        guard let inEndpoint = endpoints.first(where: { $0.address & 0x80 != 0 }),
              let outEndpoint = endpoints.first(where: { $0.address & 0x80 == 0 }) else {
            interface.destroy()
            throw MTPError.noInterface
        }
        bulkIn = try interface.copyPipe(withAddress: Int(inEndpoint.address))
        bulkOut = try interface.copyPipe(withAddress: Int(outEndpoint.address))
        maxPacketSize = max(64, Int(inEndpoint.maxPacketSize))
    }

    private struct Endpoint {
        var address: UInt8
        var attributes: UInt8
        var maxPacketSize: UInt16
    }

    /// Walks the raw configuration descriptor to find the bulk endpoints of our interface.
    private static func bulkEndpoints(of interface: IOUSBHostInterface) -> [Endpoint] {
        let config = interface.configurationDescriptor
        let ours = interface.interfaceDescriptor.pointee
        let total = Int(UInt16(littleEndian: config.pointee.wTotalLength))
        let raw = UnsafeRawPointer(config)

        var endpoints: [Endpoint] = []
        var offset = 0
        var inOurInterface = false
        while offset + 2 <= total {
            let length = Int(raw.load(fromByteOffset: offset, as: UInt8.self))
            let type = raw.load(fromByteOffset: offset + 1, as: UInt8.self)
            guard length >= 2, offset + length <= total else { break }
            switch type {
            case 0x04 where length >= 9: // interface
                let number = raw.load(fromByteOffset: offset + 2, as: UInt8.self)
                let alternate = raw.load(fromByteOffset: offset + 3, as: UInt8.self)
                inOurInterface = number == ours.bInterfaceNumber && alternate == ours.bAlternateSetting
            case 0x05 where inOurInterface && length >= 7: // endpoint
                let attributes = raw.load(fromByteOffset: offset + 3, as: UInt8.self)
                if attributes & 0x03 == 0x02 { // bulk
                    let lo = UInt16(raw.load(fromByteOffset: offset + 4, as: UInt8.self))
                    let hi = UInt16(raw.load(fromByteOffset: offset + 5, as: UInt8.self))
                    endpoints.append(Endpoint(address: raw.load(fromByteOffset: offset + 2, as: UInt8.self),
                                              attributes: attributes, maxPacketSize: (hi << 8 | lo) & 0x7FF))
                }
            default:
                break
            }
            offset += length
        }
        return endpoints
    }

    private func buffer(length: Int) throws -> NSMutableData {
        if let cached = buffers[length] { return cached }
        if buffers.count >= 8 { buffers.removeAll(keepingCapacity: true) }
        let fresh = try interface.ioData(withCapacity: length)
        buffers[length] = fresh
        return fresh
    }

    func write(_ data: Data, timeout: TimeInterval) throws {
        var transferred = 0
        if data.isEmpty {
            try bulkOut.__sendIORequest(with: nil, bytesTransferred: &transferred, completionTimeout: timeout)
            return
        }
        let buffer = try buffer(length: data.count)
        data.copyBytes(to: buffer.mutableBytes.assumingMemoryBound(to: UInt8.self), count: data.count)
        try bulkOut.__sendIORequest(with: buffer, bytesTransferred: &transferred, completionTimeout: timeout)
        guard transferred == data.count else { throw MTPError.shortRead }
    }

    func read(maxLength: Int, timeout: TimeInterval) throws -> Data {
        // Reads must be a multiple of the packet size or the host controller reports an overrun.
        let length = max(1, (maxLength + maxPacketSize - 1) / maxPacketSize) * maxPacketSize
        let buffer = try buffer(length: length)
        var transferred = 0
        try bulkIn.__sendIORequest(with: buffer, bytesTransferred: &transferred, completionTimeout: timeout)
        return Data(bytes: buffer.bytes, count: transferred)
    }

    func drain() {
        for _ in 0 ..< 32 {
            guard let data = try? read(maxLength: 64 * 1024, timeout: 0.2) else { break }
            if data.isEmpty { continue }
        }
        recover()
    }

    func cancelPendingIO() {
        try? bulkIn.__abort(with: .asynchronous)
        try? bulkOut.__abort(with: .asynchronous)
    }

    func recover() {
        try? bulkIn.clearStall()
        try? bulkOut.clearStall()
    }

    func close() {
        guard !isClosed else { return }
        isClosed = true
        try? bulkIn.__abort(with: .synchronous)
        try? bulkOut.__abort(with: .synchronous)
        interface.destroy()
    }

    deinit { close() }
}
