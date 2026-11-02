import AppKit
import Foundation
import Observation
import os
import SwiftUI

/// Discovers Kindles over both transports and owns their lifecycle.
@Observable
final class DeviceManager {
    private(set) var devices: [KindleDevice] = []
    /// True while an MTP Kindle has been seen but its session isn't open yet.
    private(set) var isConnectingUSB = false
    var lastError: String?

    @ObservationIgnored private let usbMonitor = USBDeviceMonitor()
    @ObservationIgnored private var usbTask: Task<Void, Never>?
    @ObservationIgnored private var observers: [NSObjectProtocol] = []
    @ObservationIgnored private let log = Logger(subsystem: "app.kind", category: "devices")

    func start() {
        guard usbTask == nil else { return }
        // Xcode previews, code snippets and UI tests launch a copy of the app; never let those grab the Kindle.
        let process = ProcessInfo.processInfo
        guard process.environment["XCODE_RUNNING_FOR_PREVIEWS"] != "1",
              !process.arguments.contains("-KindDisableDevices") else { return }
        let center = NSWorkspace.shared.notificationCenter
        observers.append(center.addObserver(forName: NSWorkspace.didMountNotification, object: nil, queue: .main) { [weak self] note in
            let url = note.userInfo?[NSWorkspace.volumeURLUserInfoKey] as? URL
            MainActor.assumeIsolated { if let url { self?.volumeMounted(url) } }
        })
        observers.append(center.addObserver(forName: NSWorkspace.didUnmountNotification, object: nil, queue: .main) { [weak self] note in
            let url = note.userInfo?[NSWorkspace.volumeURLUserInfoKey] as? URL
            MainActor.assumeIsolated { if let url { self?.volumeUnmounted(url) } }
        })
        observers.append(NotificationCenter.default.addObserver(forName: NSApplication.willTerminateNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.closeAllSessions() }
        })
        let volumes = FileManager.default.mountedVolumeURLs(includingResourceValuesForKeys: nil, options: [.skipHiddenVolumes]) ?? []
        volumes.forEach(volumeMounted)

        usbMonitor.start()
        let events = usbMonitor.events
        usbTask = Task { [weak self] in
            for await event in events {
                guard let self else { return }
                switch event {
                case let .attached(candidate): await self.connectMTP(candidate)
                case let .detached(registryID): await self.detachMTP(registryID)
                }
            }
        }
    }

    // MARK: Mass storage

    nonisolated static func isKindleVolume(_ url: URL) -> Bool {
        let fm = FileManager.default
        var isDirectory: ObjCBool = false
        guard fm.fileExists(atPath: url.appendingPathComponent("documents").path, isDirectory: &isDirectory), isDirectory.boolValue,
              fm.fileExists(atPath: url.appendingPathComponent("system").path) else { return false }
        return url.lastPathComponent.localizedCaseInsensitiveContains("kindle")
            || fm.fileExists(atPath: url.appendingPathComponent("system/version.txt").path)
            || fm.fileExists(atPath: url.appendingPathComponent("system/thumbnails").path)
    }

    private func volumeMounted(_ url: URL) {
        guard Self.isKindleVolume(url), !devices.contains(where: { $0.volumeURL == url }) else { return }
        let values = try? url.resourceValues(forKeys: [.volumeUUIDStringKey, .volumeNameKey])
        let serial = (try? String(contentsOf: url.appendingPathComponent("system/version.txt"), encoding: .utf8))
            .flatMap(Self.serial(fromVersionFile:)) ?? ""
        let device = KindleDevice(id: serial.isEmpty ? (values?.volumeUUIDString ?? url.path) : serial,
                                  name: values?.volumeName ?? "Kindle",
                                  model: "Kindle",
                                  serialNumber: serial,
                                  connection: .massStorage(url))
        withAnimation { devices.append(device) }
        device.attach(MassStorageFileSystem(root: url))
        log.notice("Kindle volume mounted at \(url.path, privacy: .public)")
    }

    nonisolated private static func serial(fromVersionFile text: String) -> String? {
        text.range(of: "[0-9A-Z]{16}", options: .regularExpression).map { String(text[$0]) }
    }

    private func volumeUnmounted(_ url: URL) {
        guard let device = devices.first(where: { $0.volumeURL == url }) else { return }
        Task { await device.disconnect() }
        withAnimation { devices.removeAll { $0 === device } }
    }

    // MARK: MTP

    private func connectMTP(_ candidate: USBDeviceMonitor.Candidate) async {
        let id = candidate.serialNumber.isEmpty ? "usb-\(candidate.registryID)" : candidate.serialNumber
        guard !devices.contains(where: { $0.id == id }) else { return }
        log.notice("Kindle MTP interface found: \(candidate.productName, privacy: .public) (registry \(candidate.registryID))")
        isConnectingUSB = true
        defer { isConnectingUSB = false }

        let device = KindleDevice(id: id, name: candidate.productName, model: candidate.productName,
                                  serialNumber: candidate.serialNumber, connection: .mtp(registryID: candidate.registryID))
        withAnimation { devices.append(device) }
        await openSession(for: device)
    }

    /// Opens the MTP session, retrying while the Kindle is busy. Image Capture can grab a
    /// freshly attached device for a moment, and apps like Android File Transfer or OpenMTP
    /// hold it for as long as they run, so we keep trying quietly for a while and then
    /// tell the user who to quit.
    private func openSession(for device: KindleDevice) async {
        guard case let .mtp(registryID) = device.connection else { return }
        isConnectingUSB = true
        defer { isConnectingUSB = false }
        device.phase = .connecting

        let deadline = ContinuousClock.now + .seconds(30)
        var attempt = 0
        while devices.contains(where: { $0 === device }) {
            do {
                let fileSystem = try await Self.openMTP(registryID: registryID)
                if let info = fileSystem.deviceInfo {
                    if !info.model.isEmpty { device.model = info.model }
                    if device.serialNumber.isEmpty { device.serialNumber = info.serialNumber }
                }
                device.attach(fileSystem)
                log.notice("MTP session open with \(device.model, privacy: .public)")
                return
            } catch {
                let busy = Self.isExclusiveAccessError(error)
                log.error("MTP connect attempt \(attempt) failed (busy: \(busy)): \(error.localizedDescription, privacy: .public)")
                if busy {
                    device.phase = .failed(KindleDevice.busyMessage)
                }
                guard ContinuousClock.now < deadline else {
                    if !busy { device.phase = .failed("Couldn't talk to the Kindle: \(error.localizedDescription)") }
                    return
                }
                attempt += 1
                try? await Task.sleep(for: .milliseconds(min(3000, 500 * attempt)))
            }
        }
    }

    /// Retries a Kindle whose session couldn't be opened (e.g. after quitting another MTP app).
    func reconnect(_ device: KindleDevice) {
        guard case .mtp = device.connection, device.fileSystem == nil else {
            device.refresh()
            return
        }
        Task { await openSession(for: device) }
    }

    nonisolated private static func isExclusiveAccessError(_ error: Error) -> Bool {
        let nsError = error as NSError
        // kIOReturnExclusiveAccess, or IOUSBHost's generic init failure that wraps it.
        return nsError.code == Int(Int32(bitPattern: 0xE000_02C5))
            || nsError.code == Int(Int32(bitPattern: 0xE000_02C9))
            || nsError.localizedDescription.localizedCaseInsensitiveContains("exclusive")
    }

    nonisolated private static func openMTP(registryID: UInt64) async throws -> MTPFileSystem {
        guard let service = USBDeviceMonitor.service(for: registryID) else { throw KindleError.disconnected }
        defer { IOObjectRelease(service) }
        let transport = try USBMTPTransport(interfaceService: service)
        return try await MTPFileSystem.connect(transport: transport)
    }

    private func detachMTP(_ registryID: UInt64) async {
        guard let device = devices.first(where: { $0.connection == .mtp(registryID: registryID) }) else { return }
        await device.disconnect()
        withAnimation { devices.removeAll { $0 === device } }
    }

    /// A Kindle keeps an MTP session open until told otherwise, so always say goodbye on quit.
    private func closeAllSessions() {
        for device in devices {
            (device.fileSystem as? MTPFileSystem)?.closeImmediately()
        }
    }

    // MARK: Eject

    func eject(_ device: KindleDevice) async {
        await device.disconnect()
        if let url = device.volumeURL {
            do {
                try NSWorkspace.shared.unmountAndEjectDevice(at: url)
            } catch {
                lastError = error.localizedDescription
                return
            }
        }
        withAnimation { devices.removeAll { $0 === device } }
    }

    #if DEBUG
    /// Lets SwiftUI previews show a populated library without hardware.
    func addPreviewDevice(_ device: KindleDevice) {
        devices.append(device)
    }
    #endif
}
