import SwiftUI

struct BookCard: View {
    let book: Book
    let coverKey: String
    let isSelected: Bool

    @State private var isHovering = false

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            CoverView(book: book, coverKey: coverKey)
                .overlay(alignment: .topTrailing) {
                    FormatBadge(format: book.format)
                        .padding(6)
                        .opacity(isHovering || book.format == .pdf ? 1 : 0)
                }
                .overlay(alignment: .topLeading) {
                    if book.readingState == .finished {
                        Image(systemName: "checkmark.circle.fill")
                            .symbolRenderingMode(.palette)
                            .foregroundStyle(.white, .green)
                            .font(.title3)
                            .shadow(radius: 2)
                            .padding(6)
                    }
                }
                .padding(isSelected ? 3 : 0)
                .background {
                    if isSelected {
                        RoundedRectangle(cornerRadius: Theme.coverCorner + 3)
                            .strokeBorder(Color.accentColor, lineWidth: 3)
                    }
                }
                .shadow(color: .black.opacity(isHovering ? 0.32 : 0.18), radius: isHovering ? 14 : 6, y: isHovering ? 10 : 4)
                .scaleEffect(isHovering ? 1.03 : 1, anchor: .bottom)
                .rotation3DEffect(.degrees(isHovering ? 2 : 0), axis: (x: 1, y: 0, z: 0), anchor: .bottom)

            VStack(alignment: .leading, spacing: 2) {
                Text(book.title)
                    .font(.system(.callout, weight: .semibold))
                    .lineLimit(2)
                if !book.authorLine.isEmpty {
                    Text(book.authorLine)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
                if let fraction = book.progress?.fraction, book.readingState == .reading {
                    Text(fraction, format: .percent.precision(.fractionLength(0)))
                        .font(.caption2.monospacedDigit().weight(.medium))
                        .foregroundStyle(Color.accentColor)
                }
            }
            .redacted(reason: book.isEnriched ? [] : .placeholder)
        }
        .contentShape(.rect)
        .onHover { hovering in
            withAnimation(.spring(duration: 0.3, bounce: 0.3)) { isHovering = hovering }
        }
    }
}

/// Large cover with progress for the "Continue Reading" shelf.
struct ShelfCard: View {
    let book: Book
    let coverKey: String

    @State private var isHovering = false

    var body: some View {
        HStack(spacing: 16) {
            CoverView(book: book, coverKey: coverKey, showsProgress: false)
                .frame(width: 86)
                .shadow(color: .black.opacity(0.25), radius: 8, y: 5)
            VStack(alignment: .leading, spacing: 6) {
                Text(book.title)
                    .font(.system(.headline, design: .serif))
                    .lineLimit(2)
                if !book.authorLine.isEmpty {
                    Text(book.authorLine).font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
                }
                Spacer(minLength: 0)
                if let fraction = book.progress?.fraction {
                    ProgressView(value: fraction)
                        .tint(.accentColor)
                    HStack {
                        Text(fraction, format: .percent.precision(.fractionLength(0)))
                        Spacer()
                        if let date = book.progress?.lastRead {
                            Text(date, format: .relative(presentation: .named))
                        }
                    }
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
                } else if let date = book.progress?.lastRead {
                    Text("Opened \(date, format: .relative(presentation: .named))")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            .padding(.vertical, 6)
        }
        .padding(12)
        .frame(width: 330, height: 153)
        .background {
            RoundedRectangle(cornerRadius: Theme.cardCorner)
                .fill(.background.secondary)
                .shadow(color: .black.opacity(isHovering ? 0.14 : 0.06), radius: isHovering ? 14 : 6, y: 4)
        }
        .scaleEffect(isHovering ? 1.015 : 1)
        .onHover { hovering in withAnimation(.spring(duration: 0.3)) { isHovering = hovering } }
    }
}
