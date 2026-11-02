import Foundation
import Observation
import os
import SwiftUI

@Observable
final class Transfer: Identifiable {
    enum Kind { case upload, download, edit, delete }
    enum State: Equatable { case queued, running, finished, failed(String) }

    let id = UUID()
    let kind: Kind
    let name: String
    var fraction: Double = 0
    var state: State = .queued

    init(kind: Kind, name: String) {
        self.kind = kind
        self.name = name
    }

    var symbol: String {
        switch kind {
        case .upload: "arrow.up.circle.fill"
        case .download: "arrow.down.circle.fill"
        case .edit: "pencil.circle.fill"
        case .delete: "trash.circle.fill"
        }
    }

    var verb: String {
        switch kind {
        case .upload: "Sending to Kindle"
        case .download: "Saving to Mac"
        case .edit: "Updating metadata"
        case .delete: "Removing"
        }
    }
}

/// Serial queue of device operations with observable per-item progress.
@Observable
final class TransferCenter {
    private(set) var transfers: [Transfer] = []

    var active: [Transfer] { transfers.filter { $0.state == .running || $0.state == .queued } }
    var isBusy: Bool { !active.isEmpty }

    var overallFraction: Double {
        let items = transfers
        guard !items.isEmpty else { return 0 }
        return items.reduce(0) { $0 + ($1.state == .finished ? 1 : $1.fraction) } / Double(items.count)
    }

    @ObservationIgnored private var tail: Task<Void, Never>?

    /// Enqueues work; operations on the same Kindle must not interleave, so they run one at a time.
    @discardableResult
    func enqueue(_ kind: Transfer.Kind, name: String,
                 _ work: @escaping @MainActor (_ progress: @escaping TransferProgress) async throws -> Void) -> Transfer {
        let transfer = Transfer(kind: kind, name: name)
        withAnimation(.snappy) { transfers.append(transfer) }
        let previous = tail
        tail = Task { [weak self] in
            await previous?.value
            transfer.state = .running
            let progress = Self.progressHandler(for: transfer)
            do {
                try await work(progress)
                withAnimation(.snappy) {
                    transfer.fraction = 1
                    transfer.state = .finished
                }
            } catch is CancellationError {
                transfer.state = .failed("Cancelled")
            } catch {
                transfer.state = .failed(error.localizedDescription)
            }
            self?.scheduleCleanup()
        }
        return transfer
    }

    /// Progress arrives on a background queue for every chunk; hop to the main actor and throttle.
    private static func progressHandler(for transfer: Transfer) -> TransferProgress {
        let lastUpdate = OSAllocatedUnfairLock(initialState: Date.distantPast)
        return { done, total in
            guard total > 0 else { return }
            let now = Date()
            let shouldPublish = lastUpdate.withLock { last in
                if now.timeIntervalSince(last) < 0.05 && done < total { return false }
                last = now
                return true
            }
            guard shouldPublish else { return }
            let fraction = Double(done) / Double(total)
            Task { @MainActor in transfer.fraction = fraction }
        }
    }

    private func scheduleCleanup() {
        guard !isBusy else { return }
        Task { [weak self] in
            try? await Task.sleep(for: .seconds(4))
            guard let self, !self.isBusy else { return }
            withAnimation(.smooth) {
                self.transfers.removeAll { $0.state == .finished }
            }
        }
    }

    func dismiss(_ transfer: Transfer) {
        withAnimation(.snappy) { transfers.removeAll { $0.id == transfer.id } }
    }

    func clearFinished() {
        withAnimation(.snappy) { transfers.removeAll { $0.state != .running && $0.state != .queued } }
    }
}

