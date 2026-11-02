import AppKit
import CryptoKit
import Foundation
import Observation
import os

/// Looks for newer Kind releases on GitHub and, when asked, downloads the new disk image.
///
/// Kind ships as an ad-hoc signed DMG on GitHub Releases rather than through the App Store, so it
/// checks for itself: shortly after launch, then twice a day while it stays open. Installing stays
/// in the user's hands: the verified DMG is saved to Downloads and opened, and the user drags the
/// new copy over the old one.
@Observable
final class UpdateChecker {
    /// `owner/name` of the GitHub repository whose releases are checked.
    static let repository = "buzzer-re/FutureGadgets"

    enum Download: Equatable {
        case idle
        case downloading(fraction: Double?)
        case verifying
        /// Saved (and opened) at this URL.
        case ready(URL)
        case failed(String)

        var isActive: Bool {
            switch self {
            case .downloading, .verifying: true
            default: false
            }
        }
    }

    /// Result of a check the user started from the menu, shown as an alert.
    enum Notice: Equatable {
        case upToDate(AppVersion)
        case failed(String)
    }

    let currentVersion: AppVersion?
    /// The newest published release, when it's newer than the running app.
    var latest: Release?
    private(set) var isChecking = false
    private(set) var download: Download = .idle
    var isShowingDetails = false
    var notice: Notice?

    /// A release the user chose to skip. Automatic checks don't announce it; manual checks still do.
    private(set) var skippedTag: String? = UserDefaults.standard.string(forKey: "skippedReleaseTag") {
        didSet { UserDefaults.standard.set(skippedTag, forKey: "skippedReleaseTag") }
    }

    @ObservationIgnored private var schedule: Task<Void, Never>?
    @ObservationIgnored private var downloadTask: Task<Void, Never>?
    @ObservationIgnored private let log = Logger(subsystem: "app.kind", category: "updates")

    private static let firstCheckDelay: Duration = .seconds(3)
    private static let checkInterval: Duration = .seconds(12 * 60 * 60)

    init(currentVersion: AppVersion? = Bundle.main.shortVersion.flatMap(AppVersion.init)) {
        self.currentVersion = currentVersion
    }

    /// The release to advertise in the sidebar: newer than this build and not skipped.
    var announced: Release? {
        guard let latest, latest.tag != skippedTag else { return nil }
        return latest
    }

    func start() {
        guard schedule == nil else { return }
        // Previews, tests and code snippets run copies of the app; keep them off the network.
        let process = ProcessInfo.processInfo
        guard process.environment["XCODE_RUNNING_FOR_PREVIEWS"] != "1",
              process.environment["XCTestConfigurationFilePath"] == nil,
              !process.arguments.contains("-KindDisableUpdateCheck") else { return }
        schedule = Task { [weak self] in
            try? await Task.sleep(for: Self.firstCheckDelay)
            while !Task.isCancelled {
                await self?.check(userInitiated: false)
                try? await Task.sleep(for: Self.checkInterval)
            }
        }
    }

    /// Asks GitHub for the latest release. Automatic checks fail silently. Checks the user started
    /// report "up to date" or the error, and open the details when an update exists.
    func check(userInitiated: Bool) async {
        guard !isChecking else { return }
        isChecking = true
        defer { isChecking = false }
        do {
            let release = try await Self.fetchLatestRelease()
            guard let current = currentVersion, let version = release.version, version > current else {
                latest = nil
                log.notice("No update: latest release is \(release.tag, privacy: .public)")
                if userInitiated, let currentVersion { notice = .upToDate(currentVersion) }
                return
            }
            if latest?.tag != release.tag, !download.isActive { download = .idle }
            latest = release
            log.notice("Update available: \(release.tag, privacy: .public) (running \(current.description, privacy: .public))")
            if userInitiated { isShowingDetails = true }
        } catch {
            log.error("Update check failed: \(error.localizedDescription, privacy: .public)")
            if userInitiated { notice = .failed(error.localizedDescription) }
        }
    }

    func skip(_ release: Release) {
        skippedTag = release.tag
        isShowingDetails = false
    }

    /// Downloads the release's DMG into Downloads, verifies its checksum and opens it in Finder.
    /// Releases without a DMG open their GitHub page instead.
    func downloadUpdate() {
        guard let release = latest, !download.isActive else { return }
        guard let asset = release.diskImage else {
            NSWorkspace.shared.open(release.pageURL)
            return
        }
        download = .downloading(fraction: nil)
        downloadTask = Task { [weak self] in
            do {
                let file = try await FileDownload.run(asset.downloadURL, expectedSize: asset.size) { fraction in
                    Task { @MainActor in
                        guard let self, case .downloading = self.download else { return }
                        self.download = .downloading(fraction: fraction)
                    }
                }
                self?.download = .verifying
                if let checksum = release.diskImageChecksum {
                    try await Self.verify(file, against: checksum)
                }
                let saved = try Self.moveToDownloads(file, named: asset.name)
                self?.log.notice("Downloaded \(asset.name, privacy: .public)")
                self?.download = .ready(saved)
                NSWorkspace.shared.open(saved)
            } catch is CancellationError {
                self?.download = .idle
            } catch let error as URLError where error.code == .cancelled {
                self?.download = .idle
            } catch {
                self?.log.error("Update download failed: \(error.localizedDescription, privacy: .public)")
                self?.download = .failed(error.localizedDescription)
            }
        }
    }

    func cancelDownload() {
        downloadTask?.cancel()
        downloadTask = nil
        download = .idle
    }

    #if DEBUG
    func previewDownload(_ state: Download) { download = state }
    #endif

    // MARK: - Networking

    nonisolated static func fetchLatestRelease() async throws -> Release {
        var request = URLRequest(url: URL(string: "https://api.github.com/repos/\(repository)/releases/latest")!)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        request.setValue("2022-11-28", forHTTPHeaderField: "X-GitHub-Api-Version")
        request.setValue("Kind/\(Bundle.main.shortVersion ?? "dev")", forHTTPHeaderField: "User-Agent")
        request.cachePolicy = .reloadIgnoringLocalCacheData
        request.timeoutInterval = 20

        let (data, response) = try await URLSession.shared.data(for: request)
        switch (response as? HTTPURLResponse)?.statusCode ?? 0 {
        case 200: return try Release.decode(data)
        case 404: throw UpdateError.noReleases
        case 403, 429: throw UpdateError.rateLimited
        case let status: throw UpdateError.unexpectedResponse(status)
        }
    }

    nonisolated private static func verify(_ file: URL, against checksum: Release.Asset) async throws {
        let (data, _) = try await URLSession.shared.data(from: checksum.downloadURL)
        guard let expected = Release.parseChecksum(String(decoding: data, as: UTF8.self)) else {
            throw UpdateError.damagedDownload
        }
        let actual = try SHA256.hash(data: Data(contentsOf: file, options: .mappedIfSafe))
            .map { String(format: "%02x", $0) }.joined()
        guard actual == expected else { throw UpdateError.damagedDownload }
    }

    nonisolated private static func moveToDownloads(_ file: URL, named name: String) throws -> URL {
        let manager = FileManager.default
        let folder = manager.urls(for: .downloadsDirectory, in: .userDomainMask).first ?? manager.temporaryDirectory
        let destination = folder.appending(path: name)
        if manager.fileExists(atPath: destination.path(percentEncoded: false)) {
            try manager.removeItem(at: destination)
        }
        try manager.moveItem(at: file, to: destination)
        return destination
    }
}

nonisolated enum UpdateError: LocalizedError {
    case noReleases
    case rateLimited
    case unexpectedResponse(Int)
    case damagedDownload

    var errorDescription: String? {
        switch self {
        case .noReleases: "No Kind releases have been published yet."
        case .rateLimited: "GitHub is limiting update checks right now. Try again in an hour."
        case let .unexpectedResponse(status): "GitHub sent an unexpected response (HTTP \(status)). Try again later."
        case .damagedDownload: "The download was damaged: its checksum doesn't match the release. Try downloading it again."
        }
    }
}

nonisolated extension Bundle {
    var shortVersion: String? { infoDictionary?["CFBundleShortVersionString"] as? String }
}

/// Downloads one file and reports progress. The async `URLSession.download` doesn't report
/// progress, so this wraps a delegate-based download task.
///
/// The session's delegate queue is serial, so the mutable state is only touched from one queue
/// (plus once before the task starts).
nonisolated private final class FileDownload: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
    private let expectedSize: Int64
    private let onProgress: @Sendable (Double) -> Void
    private var continuation: CheckedContinuation<URL, Error>?
    private var result: Result<URL, Error>?

    private init(expectedSize: Int64, onProgress: @escaping @Sendable (Double) -> Void) {
        self.expectedSize = expectedSize
        self.onProgress = onProgress
    }

    /// Downloads `url` to a temporary file the caller then owns.
    static func run(_ url: URL, expectedSize: Int64, onProgress: @escaping @Sendable (Double) -> Void) async throws -> URL {
        let delegate = FileDownload(expectedSize: expectedSize, onProgress: onProgress)
        let session = URLSession(configuration: .ephemeral, delegate: delegate, delegateQueue: nil)
        defer { session.finishTasksAndInvalidate() }
        let task = session.downloadTask(with: url)
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                delegate.continuation = continuation
                task.resume()
            }
        } onCancel: {
            task.cancel()
        }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
                    totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        let total = totalBytesExpectedToWrite > 0 ? totalBytesExpectedToWrite : expectedSize
        guard total > 0 else { return }
        onProgress(min(1, Double(totalBytesWritten) / Double(total)))
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        // `location` is deleted when this returns, so move it somewhere we own first.
        if let status = (downloadTask.response as? HTTPURLResponse)?.statusCode, status != 200 {
            result = .failure(UpdateError.unexpectedResponse(status))
            return
        }
        let destination = FileManager.default.temporaryDirectory.appending(path: "\(UUID().uuidString).dmg")
        result = Result { try FileManager.default.moveItem(at: location, to: destination); return destination }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error {
            continuation?.resume(throwing: error)
        } else {
            continuation?.resume(with: result ?? .failure(URLError(.unknown)))
        }
        continuation = nil
    }
}
