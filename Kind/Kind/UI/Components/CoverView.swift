import SwiftUI

/// A book cover: the cached cover image when there is one, otherwise a typeset placeholder.
struct CoverView: View {
    let book: Book
    let coverKey: String
    var showsProgress = true

    @State private var image: NSImage?

    var body: some View {
        ZStack {
            if let image {
                Image(nsImage: image)
                    .resizable()
                    .aspectRatio(contentMode: .fill)
                    .transition(.opacity)
            } else {
                GeneratedCover(title: book.title, author: book.authorLine, format: book.format)
                    .transition(.opacity)
            }
        }
        .aspectRatio(Theme.coverAspect, contentMode: .fit)
        .clipShape(.rect(cornerRadius: Theme.coverCorner))
        .overlay(alignment: .bottom) {
            if showsProgress, let fraction = book.progress?.fraction, fraction > 0.005 {
                ProgressStripe(fraction: fraction, finished: book.readingState == .finished)
            }
        }
        .overlay {
            // Hairline edge + spine shading make flat images read as physical books.
            RoundedRectangle(cornerRadius: Theme.coverCorner)
                .strokeBorder(.black.opacity(0.12), lineWidth: 0.5)
            LinearGradient(colors: [.black.opacity(0.18), .clear, .white.opacity(0.06), .clear],
                           startPoint: .leading, endPoint: .init(x: 0.12, y: 0.5))
                .clipShape(.rect(cornerRadius: Theme.coverCorner))
                .allowsHitTesting(false)
        }
        .task(id: coverKey) {
            let loaded = book.hasEmbeddedCover ? await CoverStore.shared.image(for: coverKey) : nil
            withAnimation(.easeOut(duration: 0.25)) { image = loaded }
        }
    }
}

private struct ProgressStripe: View {
    let fraction: Double
    let finished: Bool

    var body: some View {
        GeometryReader { proxy in
            ZStack(alignment: .leading) {
                Rectangle().fill(.black.opacity(0.35))
                Rectangle()
                    .fill(finished ? Color.green : Color.accentColor)
                    .frame(width: proxy.size.width * fraction)
            }
        }
        .frame(height: 4)
    }
}

/// Typeset cover for books without artwork (most personal documents and PDFs over MTP
/// before they're processed). Deterministic colors per title keep the grid calm.
struct GeneratedCover: View {
    let title: String
    let author: String
    let format: BookFormat

    var body: some View {
        let (dark, light) = Theme.palette(for: title)
        GeometryReader { proxy in
            let width = proxy.size.width
            ZStack {
                LinearGradient(colors: [light, dark], startPoint: .topLeading, endPoint: .bottomTrailing)
                // Subtle frame, like a cloth-bound hardcover.
                RoundedRectangle(cornerRadius: 2)
                    .strokeBorder(.white.opacity(0.25), lineWidth: max(0.5, width * 0.006))
                    .padding(width * 0.07)
                VStack(spacing: width * 0.05) {
                    Image(systemName: format == .pdf ? "doc.richtext" : "book.closed")
                        .font(.system(size: width * 0.11, weight: .light))
                        .foregroundStyle(.white.opacity(0.55))
                    Text(title)
                        .font(.system(size: max(9, width * 0.1), weight: .semibold, design: .serif))
                        .multilineTextAlignment(.center)
                        .lineLimit(5)
                        .minimumScaleFactor(0.6)
                        .foregroundStyle(.white)
                    if !author.isEmpty {
                        Rectangle().fill(.white.opacity(0.4)).frame(width: width * 0.18, height: 1)
                        Text(author.uppercased())
                            .font(.system(size: max(7, width * 0.055), weight: .medium))
                            .tracking(width * 0.006)
                            .lineLimit(2)
                            .multilineTextAlignment(.center)
                            .foregroundStyle(.white.opacity(0.8))
                    }
                }
                .padding(width * 0.14)
            }
        }
    }
}

#Preview("Generated covers") {
    HStack(spacing: 16) {
        GeneratedCover(title: "The Left Hand of Darkness", author: "Ursula K. Le Guin", format: .azw3)
        GeneratedCover(title: "Quarterly Report 2026", author: "", format: .pdf)
        GeneratedCover(title: "Dune", author: "Frank Herbert", format: .mobi)
    }
    .frame(width: 520, height: 240)
    .padding()
}
