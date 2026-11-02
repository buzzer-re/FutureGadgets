import SwiftUI

/// Empty state shown until a Kindle is plugged in.
struct ConnectKindleView: View {
    let isConnecting: Bool
    @State private var wave = false

    var body: some View {
        VStack(spacing: 28) {
            ZStack {
                ForEach(0 ..< 3, id: \.self) { ring in
                    Circle()
                        .stroke(Color.accentColor.opacity(0.35), lineWidth: 1.5)
                        .frame(width: 150, height: 150)
                        .scaleEffect(wave ? 1.9 : 0.8)
                        .opacity(wave ? 0 : 0.9)
                        .animation(.easeOut(duration: 2.6).repeatForever(autoreverses: false).delay(Double(ring) * 0.85), value: wave)
                }
                KindleGlyph(screenGlow: isConnecting ? 0.8 : 0)
                    .frame(width: 96)
                    .shadow(color: .black.opacity(0.25), radius: 18, y: 10)
                    .rotation3DEffect(.degrees(wave ? -6 : 6), axis: (x: 0, y: 1, z: 0))
                    .animation(.easeInOut(duration: 3).repeatForever(), value: wave)
            }
            .frame(height: 230)

            VStack(spacing: 10) {
                Text(isConnecting ? "Talking to your Kindle…" : "Connect your Kindle")
                    .font(.system(.largeTitle, design: .serif, weight: .semibold))
                    .contentTransition(.opacity)
                Text("Plug it in with a USB cable. Kind reads newer Kindles over MTP directly — no Android File Transfer or other helper apps — and older models as a USB drive.")
                    .font(.title3)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: 520)
            }

            HStack(spacing: 22) {
                Feature(symbol: "books.vertical", title: "Every book", detail: "Kindle books, PDFs and documents")
                Feature(symbol: "chart.bar.fill", title: "Progress", detail: "Read straight from the Kindle")
                Feature(symbol: "square.and.pencil", title: "Metadata", detail: "Covers, titles, authors")
            }
            .padding(.top, 8)
        }
        .padding(40)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .onAppear { wave = true }
        .animation(.smooth, value: isConnecting)
    }
}

private struct Feature: View {
    let symbol: String
    let title: String
    let detail: String

    var body: some View {
        VStack(spacing: 6) {
            Image(systemName: symbol)
                .font(.title2)
                .foregroundStyle(Color.accentColor)
                .frame(height: 28)
            Text(title).font(.headline)
            Text(detail).font(.caption).foregroundStyle(.secondary).multilineTextAlignment(.center)
        }
        .frame(width: 150, height: 96)
        .padding(.vertical, 10)
        .background(.quaternary.opacity(0.4), in: .rect(cornerRadius: Theme.cardCorner))
    }
}

#Preview {
    ConnectKindleView(isConnecting: false)
        .frame(width: 900, height: 640)
}
