#if DEBUG
import SwiftUI

enum PreviewSupport {
    static func book(_ title: String, _ author: String, _ format: BookFormat, progress: Double?, daysAgo: Double = 1) -> Book {
        var metadata = BookMetadata()
        metadata.title = title
        metadata.authors = author.isEmpty ? [] : [author]
        metadata.summary = "A preview description of \(title), used to check layout and typography in the inspector."
        metadata.pageCount = format == .pdf ? 212 : nil
        var book = Book(path: "documents/\(title).\(format.rawValue)", format: format, size: Int.random(in: 400_000 ... 30_000_000),
                        modified: .now.addingTimeInterval(-86_400 * 30), metadata: metadata)
        book.isEnriched = true
        if let progress {
            book.progress = ReadingProgress(fraction: progress, lastRead: .now.addingTimeInterval(-86_400 * daysAgo), highlights: 3)
        }
        return book
    }

    static func model() -> AppModel {
        let model = AppModel()
        let device = KindleDevice(id: "preview", name: "My Kindle", model: "Kindle Paperwhite",
                                  serialNumber: "", connection: .mtp(registryID: 1))
        device.phase = .ready
        device.storage = StorageInfo(capacity: 16_000_000_000, free: 9_400_000_000)
        device.batteryLevel = 72
        device.books = [
            book("The Left Hand of Darkness", "Ursula K. Le Guin", .azw3, progress: 0.62, daysAgo: 0.2),
            book("Project Hail Mary", "Andy Weir", .azw3, progress: 0.18, daysAgo: 2),
            book("Designing Data-Intensive Applications", "Martin Kleppmann", .pdf, progress: 0.41, daysAgo: 5),
            book("Dune", "Frank Herbert", .mobi, progress: 1),
            book("Piranesi", "Susanna Clarke", .azw3, progress: nil),
            book("Q3 Board Report", "", .pdf, progress: nil),
            book("The Dispossessed", "Ursula K. Le Guin", .kfx, progress: nil),
            book("Meditations", "Marcus Aurelius", .azw3, progress: 1),
            book("Recipes from Grandma", "", .txt, progress: nil),
            book("Structure and Interpretation of Computer Programs", "Abelson & Sussman", .pdf, progress: 0.07, daysAgo: 12),
        ]
        model.devices.addPreviewDevice(device)
        return model
    }
}

#Preview("Library") {
    RootView()
        .environment(PreviewSupport.model())
        .frame(width: 1240, height: 780)
}

#Preview("Inspector") {
    let model = PreviewSupport.model()
    model.selection = [model.selectedDevice!.books[0].id]
    return RootView()
        .environment(model)
        .frame(width: 1240, height: 780)
}

#Preview("Book Inspector") {
    let model = PreviewSupport.model()
    let device = model.selectedDevice!
    return BookInspector(book: device.books[0], device: device)
        .environment(model)
        .frame(width: 330, height: 900)
}

#Preview("Empty") {
    RootView()
        .environment(AppModel())
        .frame(width: 1100, height: 700)
}
#endif

#if DEBUG
#Preview("Metadata Editor") {
    let model = PreviewSupport.model()
    let device = model.selectedDevice!
    return MetadataEditor(book: device.books[0], device: device)
        .environment(model)
}
#endif
