import AppKit
import CryptoKit
import Foundation

/// Two-level cover cache: decoded images in memory, downscaled JPEGs on disk, keyed by the
/// book's content key so edits and re-uploads invalidate naturally.
actor CoverStore {
    static let shared = CoverStore()

    private let memory = NSCache<NSString, NSImage>()
    private let directory: URL

    init() {
        let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
        directory = caches.appendingPathComponent(Bundle.main.bundleIdentifier ?? "Kind").appendingPathComponent("Covers")
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        memory.countLimit = 400
    }

    nonisolated static func key(device: String, contentKey: String) -> String {
        let digest = SHA256.hash(data: Data("\(device)#\(contentKey)".utf8))
        return digest.prefix(16).map { String(format: "%02x", $0) }.joined()
    }

    private func url(for key: String) -> URL { directory.appendingPathComponent("\(key).jpg") }

    func contains(_ key: String) -> Bool {
        memory.object(forKey: key as NSString) != nil || FileManager.default.fileExists(atPath: url(for: key).path)
    }

    func image(for key: String) -> NSImage? {
        if let image = memory.object(forKey: key as NSString) { return image }
        guard let data = try? Data(contentsOf: url(for: key)), let image = NSImage(data: data) else { return nil }
        memory.setObject(image, forKey: key as NSString)
        return image
    }

    /// Stores a cover, downscaling to a size that looks crisp in the grid and inspector.
    @discardableResult
    func store(_ data: Data, for key: String) -> Bool {
        guard let jpeg = CoverEncoder.encode(data, maxDimension: 900), let image = NSImage(data: jpeg) else { return false }
        try? jpeg.write(to: url(for: key), options: .atomic)
        memory.setObject(image, forKey: key as NSString)
        return true
    }

    func remove(_ key: String) {
        memory.removeObject(forKey: key as NSString)
        try? FileManager.default.removeItem(at: url(for: key))
    }
}

/// Persists parsed metadata per device so reconnecting a Kindle shows the library instantly.
actor MetadataCache {
    private struct Entry: Codable {
        var metadata: BookMetadata
        var hasCover: Bool
    }

    private let url: URL
    private var entries: [String: Entry]
    private var isDirty = false

    init(deviceID: String) {
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent(Bundle.main.bundleIdentifier ?? "Kind")
            .appendingPathComponent("Libraries")
        try? FileManager.default.createDirectory(at: support, withIntermediateDirectories: true)
        let safe = deviceID.replacingOccurrences(of: "[^A-Za-z0-9_-]", with: "_", options: .regularExpression)
        url = support.appendingPathComponent("\(safe).json")
        entries = (try? JSONDecoder().decode([String: Entry].self, from: Data(contentsOf: url))) ?? [:]
    }

    func lookup(_ contentKey: String) -> (BookMetadata, Bool)? {
        entries[contentKey].map { ($0.metadata, $0.hasCover) }
    }

    func store(_ metadata: BookMetadata, hasCover: Bool, for contentKey: String) {
        entries[contentKey] = Entry(metadata: metadata, hasCover: hasCover)
        isDirty = true
    }

    /// Drops entries for files that no longer exist and writes to disk.
    func save(keeping liveKeys: Set<String>? = nil) {
        if let liveKeys {
            let before = entries.count
            entries = entries.filter { liveKeys.contains($0.key) }
            isDirty = isDirty || entries.count != before
        }
        guard isDirty, let data = try? JSONEncoder().encode(entries) else { return }
        try? data.write(to: url, options: .atomic)
        isDirty = false
    }
}
