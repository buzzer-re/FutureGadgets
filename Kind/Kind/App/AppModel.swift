import Foundation
import Observation
import SwiftUI

/// Window-level UI state plus the long-lived services.
@Observable
final class AppModel {
    enum ViewMode: String, CaseIterable, Identifiable {
        case grid, list
        var id: String { rawValue }
        var title: String { self == .grid ? "Covers" : "List" }
        var symbol: String { self == .grid ? "square.grid.2x2" : "list.bullet" }
    }

    let devices = DeviceManager()
    let transfers = TransferCenter()
    let updates = UpdateChecker()

    var selectedDeviceID: KindleDevice.ID?
    var filter: LibraryFilter = .all
    var sort: LibrarySort = .recent {
        didSet { UserDefaults.standard.set(sort.rawValue, forKey: "sort") }
    }
    var viewMode: ViewMode = .grid {
        didSet { UserDefaults.standard.set(viewMode.rawValue, forKey: "viewMode") }
    }
    var searchText = ""
    var selection: Set<Book.ID> = []
    var showInspector = true
    var isImporting = false
    var editingBook: Book?
    var pendingDeletion: [Book] = []
    var booksToExport: [Book] = []
    var isExporting = false

    init() {
        sort = UserDefaults.standard.string(forKey: "sort").flatMap(LibrarySort.init) ?? .recent
        viewMode = UserDefaults.standard.string(forKey: "viewMode").flatMap(ViewMode.init) ?? .grid
    }

    func start() {
        devices.start()
        updates.start()
    }

    var selectedDevice: KindleDevice? {
        if let selectedDeviceID, let device = devices.devices.first(where: { $0.id == selectedDeviceID }) { return device }
        return devices.devices.first
    }

    var actions: LibraryActions? {
        selectedDevice.map { LibraryActions(device: $0, transfers: transfers) }
    }

    /// Books after filter, search and sort.
    var visibleBooks: [Book] {
        guard let device = selectedDevice else { return [] }
        let query = searchText.trimmingCharacters(in: .whitespaces)
        let filtered = device.books.filter { book in
            filter.includes(book) && (query.isEmpty
                || book.title.localizedStandardContains(query)
                || book.authorLine.localizedStandardContains(query)
                || book.fileName.localizedStandardContains(query))
        }
        return sort.sorted(filtered)
    }

    var continueReading: [Book] {
        guard let device = selectedDevice else { return [] }
        return Array(device.books.filter { $0.readingState == .reading }
            .sorted { ($0.progress?.lastRead ?? .distantPast) > ($1.progress?.lastRead ?? .distantPast) }
            .prefix(8))
    }

    func count(for filter: LibraryFilter) -> Int {
        selectedDevice?.books.lazy.filter(filter.includes).count ?? 0
    }

    var selectedBooks: [Book] {
        guard let device = selectedDevice else { return [] }
        return selection.compactMap(device.book(withID:))
    }

    /// The book shown in the inspector: the most recent single selection.
    var focusedBook: Book? {
        selectedBooks.count == 1 ? selectedBooks.first : nil
    }

    func editSelected() {
        editingBook = focusedBook
    }

    func requestDelete(_ books: [Book]) {
        pendingDeletion = books
    }

    func confirmDelete() {
        actions?.delete(pendingDeletion)
        selection.subtract(pendingDeletion.map(\.id))
        pendingDeletion = []
    }
}
