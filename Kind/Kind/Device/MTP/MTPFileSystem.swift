import Foundation

/// `KindleFileSystem` over MTP. The actor runs on its own serial dispatch queue, so the
/// blocking USB I/O inside `MTPSession` never stalls Swift's cooperative thread pool.
actor MTPFileSystem: KindleFileSystem {
    private let queue: DispatchSerialQueue
    nonisolated var unownedExecutor: UnownedSerialExecutor { queue.asUnownedSerialExecutor() }

    private let session: MTPSession
    private var storageID: UInt32 = 0

    /// Path → object handle, filled lazily as folders are listed.
    private var handles: [String: UInt32] = [:]
    private var items: [String: RemoteItem] = [:]
    private var listedFolders: Set<String> = []

    /// Whole-file cache used when the device doesn't support partial reads.
    private var fileCache: (path: String, data: Data)?

    nonisolated let deviceInfo: MTPDeviceInfo?

    /// Opens the session. Called on the device queue since it performs blocking I/O.
    static func connect(transport: any MTPTransport) async throws -> MTPFileSystem {
        let queue = DispatchSerialQueue(label: "app.kind.mtp", qos: .userInitiated)
        let session = MTPSession(transport: transport)
        let info: MTPDeviceInfo? = try await withCheckedThrowingContinuation { continuation in
            queue.async {
                do {
                    try session.open()
                    continuation.resume(returning: session.deviceInfo)
                } catch {
                    session.close()
                    continuation.resume(throwing: error)
                }
            }
        }
        let fileSystem = MTPFileSystem(queue: queue, session: session, deviceInfo: info)
        try await fileSystem.selectStorage()
        return fileSystem
    }

    private init(queue: DispatchSerialQueue, session: MTPSession, deviceInfo: MTPDeviceInfo?) {
        self.queue = queue
        self.session = session
        self.deviceInfo = deviceInfo
    }

    private func selectStorage() throws {
        let ids = try session.storageIDs()
        // Prefer the first writable storage; Kindles expose a single "Internal Storage".
        storageID = ids.first(where: { (try? session.storageInfo($0).accessCapability == 0) ?? false }) ?? ids.first ?? 0x0001_0001
    }

    /// Closes the session while the app is terminating, when there's no time to hop through
    /// the actor. In-flight transfers are cancelled first so a running scan can't hold up quitting,
    /// and the wait is capped. Must not be called from the device queue.
    nonisolated func closeImmediately() {
        session.cancelPendingIO()
        let closed = DispatchSemaphore(value: 0)
        queue.async { [session] in
            session.close()
            closed.signal()
        }
        _ = closed.wait(timeout: .now() + 1.5)
    }

    func disconnect() {
        session.close()
        handles.removeAll()
        items.removeAll()
        listedFolders.removeAll()
    }

    func batteryLevel() -> Int? {
        guard deviceInfo?.devicePropertiesSupported.contains(MTPDeviceProperty.batteryLevel.rawValue) == true,
              let data = try? session.deviceProperty(.batteryLevel), let level = data.first else { return nil }
        return Int(level)
    }

    // MARK: Path resolution

    private func handle(for path: String) throws -> UInt32 {
        if path.isEmpty { return MTPSession.rootParent }
        if let handle = handles[path] { return handle }
        let parent = (path as NSString).deletingLastPathComponent
        try listChildren(of: parent)
        guard let handle = handles[path] else { throw KindleError.notFound(path) }
        return handle
    }

    @discardableResult
    private func listChildren(of path: String) throws -> [RemoteItem] {
        let parentHandle = try handle(for: path)
        let children = try session.objectHandles(storage: storageID, parent: parentHandle)
        var result: [RemoteItem] = []
        for child in children {
            guard let info = try? session.objectInfo(child) else { continue }
            let childPath = path.isEmpty ? info.filename : "\(path)/\(info.filename)"
            let item = RemoteItem(path: childPath, size: Int(info.compressedSize),
                                  modified: MTPDate.parse(info.dateModified), isDirectory: info.isFolder)
            handles[childPath] = child
            items[childPath] = item
            result.append(item)
        }
        listedFolders.insert(path)
        return result
    }

    // MARK: KindleFileSystem

    func list(_ path: String) throws -> [RemoteItem] {
        var result: [RemoteItem] = []
        var pending = [path]
        while let folder = pending.popLast() {
            try Task.checkCancellation()
            let children = try listChildren(of: folder)
            result.append(contentsOf: children)
            pending.append(contentsOf: children.filter(\.isDirectory).map(\.path))
        }
        return result
    }

    func read(_ path: String) throws -> Data {
        if let cached = fileCache, cached.path == path { return cached.data }
        var data = Data()
        try session.object(try handle(for: path)) { data.append($0) }
        fileCache = (path, data)
        return data
    }

    func read(_ path: String, offset: Int, length: Int) throws -> Data {
        guard length > 0 else { return Data() }
        if session.supportsPartialReads, offset <= Int(UInt32.max) {
            return try session.partialObject(try handle(for: path), offset: UInt32(offset), length: UInt32(length))
        }
        let data = try read(path)
        let end = min(data.count, offset + length)
        return offset < end ? data.subdata(in: offset ..< end) : Data()
    }

    func download(_ path: String, to destination: URL, progress: TransferProgress?) throws {
        FileManager.default.createFile(atPath: destination.path, contents: nil)
        let output = try FileHandle(forWritingTo: destination)
        defer { try? output.close() }
        try session.object(try handle(for: path), progress: progress) { chunk in
            try output.write(contentsOf: chunk)
        }
    }

    func upload(_ source: URL, to directory: String, name: String, progress: TransferProgress?) throws -> RemoteItem {
        let size = Int64((try? source.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
        if let info = try? storage(), info.free < size { throw KindleError.notEnoughSpace }
        let parent = try ensureFolder(directory)
        let path = directory.isEmpty ? name : "\(directory)/\(name)"

        // MTP has no overwrite; replace by deleting the existing object first.
        if !listedFolders.contains(directory) { try listChildren(of: directory) }
        if let existing = handles[path] {
            try session.deleteObject(existing)
            forget(path)
        }

        let format: MTPObjectFormat = source.pathExtension.lowercased() == "txt" ? .text : .undefined
        let info = MTPObjectInfo(storageID: storageID, format: format.rawValue,
                                 compressedSize: UInt32(clamping: size), parent: parent,
                                 filename: name, dateModified: MTPDate.format(.now))
        let handle = try session.sendObject(info: info, storage: storageID, parent: parent,
                                            contents: .file(source, size: size), progress: progress)
        let item = RemoteItem(path: path, size: Int(size), modified: .now, isDirectory: false)
        handles[path] = handle
        items[path] = item
        if fileCache?.path == path { fileCache = nil }
        return item
    }

    private func ensureFolder(_ path: String) throws -> UInt32 {
        if let handle = try? handle(for: path) { return handle }
        let parentPath = (path as NSString).deletingLastPathComponent
        let parent = try ensureFolder(parentPath)
        let info = MTPObjectInfo(storageID: storageID, format: MTPObjectFormat.association.rawValue,
                                 compressedSize: 0, parent: parent, associationType: 1,
                                 filename: (path as NSString).lastPathComponent)
        let handle = try session.sendObject(info: info, storage: storageID, parent: parent, contents: nil)
        handles[path] = handle
        items[path] = RemoteItem(path: path, size: 0, modified: .now, isDirectory: true)
        return handle
    }

    func delete(_ path: String) throws {
        try session.deleteObject(try handle(for: path))
        forget(path)
    }

    private func forget(_ path: String) {
        for key in handles.keys where key == path || key.hasPrefix(path + "/") {
            handles[key] = nil
            items[key] = nil
        }
        if fileCache?.path == path { fileCache = nil }
    }

    func storage() throws -> StorageInfo {
        let info = try session.storageInfo(storageID)
        return StorageInfo(capacity: Int64(clamping: info.maxCapacity), free: Int64(clamping: info.freeSpace))
    }
}
