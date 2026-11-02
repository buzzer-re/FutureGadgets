import SwiftUI

struct BookTable: View {
    @Environment(AppModel.self) private var model
    let device: KindleDevice
    let books: [Book]

    var body: some View {
        @Bindable var model = model
        Table(books, selection: $model.selection) {
            TableColumn("Title") { book in
                HStack(spacing: 10) {
                    CoverView(book: book, coverKey: device.coverKey(for: book), showsProgress: false)
                        .frame(width: 26)
                        .shadow(color: .black.opacity(0.15), radius: 1.5, y: 1)
                    VStack(alignment: .leading, spacing: 1) {
                        Text(book.title).fontWeight(.medium).lineLimit(1)
                        Text(book.fileName).font(.caption).foregroundStyle(.tertiary).lineLimit(1)
                    }
                }
                .padding(.vertical, 2)
            }
            .width(min: 220, ideal: 340)

            TableColumn("Author") { book in
                Text(book.authorLine).foregroundStyle(.secondary).lineLimit(1)
            }
            .width(min: 100, ideal: 170)

            TableColumn("Progress") { book in
                HStack(spacing: 8) {
                    ProgressView(value: book.progress?.fraction ?? 0)
                        .tint(book.readingState.tint)
                        .frame(width: 70)
                    Text(book.progress?.fraction.map { $0.formatted(.percent.precision(.fractionLength(0))) } ?? book.readingState.label)
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
            }
            .width(min: 120, ideal: 140)

            TableColumn("Last Read") { book in
                if let date = book.progress?.lastRead {
                    Text(date, format: .dateTime.day().month(.abbreviated).year())
                        .foregroundStyle(.secondary)
                } else {
                    Text("—").foregroundStyle(.tertiary)
                }
            }
            .width(min: 80, ideal: 110)

            TableColumn("Kind") { book in FormatBadge(format: book.format) }
                .width(56)

            TableColumn("Size") { book in
                Text(book.size.byteString).monospacedDigit().foregroundStyle(.secondary)
            }
            .width(min: 60, ideal: 80)
        }
        .contextMenu(forSelectionType: Book.ID.self) { ids in
            BookContextMenu(books: ids.compactMap(device.book(withID:)))
        } primaryAction: { ids in
            if ids.count == 1, let id = ids.first { model.editingBook = device.book(withID: id) }
        }
        .alternatingRowBackgrounds()
    }
}
