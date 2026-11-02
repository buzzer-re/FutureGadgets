import Foundation
import IOKit
import IOKit.usb

/// Watches the IORegistry for Kindle MTP interfaces appearing and disappearing.
///
/// Lab126 (Amazon's hardware division) uses USB vendor ID 0x1949. Kindles that speak MTP
/// expose a Still Image (class 6) interface, or a vendor-specific one named "MTP".
nonisolated final class USBDeviceMonitor: @unchecked Sendable {
    static let amazonVendorID = 0x1949

    struct Candidate: Sendable, Hashable {
        /// Stable registry ID of the interface service; used to match the detach event.
        var registryID: UInt64
        var productName: String
        var serialNumber: String
        var productID: Int
    }

    enum Event: Sendable {
        case attached(Candidate)
        case detached(registryID: UInt64)
    }

    let events: AsyncStream<Event>
    private let continuation: AsyncStream<Event>.Continuation
    private let queue = DispatchQueue(label: "app.kind.usb.monitor")
    private var port: IONotificationPortRef?
    private var matchIterator: io_iterator_t = 0
    private var terminateIterator: io_iterator_t = 0

    init() {
        (events, continuation) = AsyncStream.makeStream(bufferingPolicy: .unbounded)
    }

    func start() {
        queue.async { self.install() }
    }

    func stop() {
        queue.sync {
            if matchIterator != 0 { IOObjectRelease(matchIterator) }
            if terminateIterator != 0 { IOObjectRelease(terminateIterator) }
            if let port { IONotificationPortDestroy(port) }
            port = nil
            matchIterator = 0
            terminateIterator = 0
        }
        continuation.finish()
    }

    /// Looks up the interface service for a candidate; the caller owns the returned reference.
    static func service(for registryID: UInt64) -> io_service_t? {
        guard let matching = IORegistryEntryIDMatching(registryID) else { return nil }
        let service = IOServiceGetMatchingService(kIOMainPortDefault, matching)
        return service == 0 ? nil : service
    }

    /// USB interface matching follows the USB class-spec rules, where `idVendor` alone is not a
    /// valid key combination. So we match every interface and filter by vendor ourselves.
    private func matchingDictionary() -> CFDictionary {
        IOServiceMatching("IOUSBHostInterface")
    }

    private func install() {
        guard let port = IONotificationPortCreate(kIOMainPortDefault) else { return }
        self.port = port
        IONotificationPortSetDispatchQueue(port, queue)
        let context = Unmanaged.passUnretained(self).toOpaque()

        IOServiceAddMatchingNotification(port, kIOFirstMatchNotification, matchingDictionary(), { context, iterator in
            guard let context else { return }
            Unmanaged<USBDeviceMonitor>.fromOpaque(context).takeUnretainedValue().handleMatched(iterator)
        }, context, &matchIterator)
        handleMatched(matchIterator) // arms the notification and reports already-connected devices

        IOServiceAddMatchingNotification(port, kIOTerminatedNotification, matchingDictionary(), { context, iterator in
            guard let context else { return }
            Unmanaged<USBDeviceMonitor>.fromOpaque(context).takeUnretainedValue().handleTerminated(iterator)
        }, context, &terminateIterator)
        handleTerminated(terminateIterator)
    }

    private func handleMatched(_ iterator: io_iterator_t) {
        while case let service = IOIteratorNext(iterator), service != 0 {
            defer { IOObjectRelease(service) }
            guard Self.property(service, "idVendor") as? Int == Self.amazonVendorID,
                  Self.isMTPInterface(service) else { continue }
            var registryID: UInt64 = 0
            IORegistryEntryGetRegistryEntryID(service, &registryID)
            let candidate = Candidate(
                registryID: registryID,
                productName: Self.property(service, "USB Product Name") as? String ?? "Kindle",
                serialNumber: Self.property(service, "USB Serial Number") as? String
                    ?? Self.property(service, "kUSBSerialNumberString") as? String ?? "",
                productID: Self.property(service, "idProduct") as? Int ?? 0)
            continuation.yield(.attached(candidate))
        }
    }

    private func handleTerminated(_ iterator: io_iterator_t) {
        while case let service = IOIteratorNext(iterator), service != 0 {
            var registryID: UInt64 = 0
            IORegistryEntryGetRegistryEntryID(service, &registryID)
            IOObjectRelease(service)
            continuation.yield(.detached(registryID: registryID))
        }
    }

    private static func isMTPInterface(_ service: io_service_t) -> Bool {
        let interfaceClass = property(service, "bInterfaceClass") as? Int ?? -1
        let subclass = property(service, "bInterfaceSubClass") as? Int ?? -1
        if interfaceClass == 6 && subclass == 1 { return true } // Still Image / PTP
        if interfaceClass == 0xFF {
            let name = (property(service, "kUSBString") as? String) ?? (property(service, "USB Interface Name") as? String) ?? ""
            return name.uppercased().contains("MTP")
        }
        return false
    }

    /// Reads a property from the service or, failing that, from its parents (the device node).
    private static func property(_ service: io_service_t, _ key: String) -> Any? {
        IORegistryEntrySearchCFProperty(service, kIOServicePlane, key as CFString, kCFAllocatorDefault,
                                        IOOptionBits(kIORegistryIterateRecursively | kIORegistryIterateParents))
    }
}
