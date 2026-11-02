import SwiftUI

struct ProgressRing: View {
    let fraction: Double?
    var lineWidth: CGFloat = 8
    var tint: Color = .accentColor

    @State private var animated: Double = 0

    var body: some View {
        ZStack {
            Circle().stroke(.quaternary, lineWidth: lineWidth)
            Circle()
                .trim(from: 0, to: animated)
                .stroke(tint.gradient, style: StrokeStyle(lineWidth: lineWidth, lineCap: .round))
                .rotationEffect(.degrees(-90))
            if let fraction {
                Text(fraction, format: .percent.precision(.fractionLength(0)))
                    .font(.system(.title3, design: .rounded, weight: .semibold))
                    .monospacedDigit()
                    .contentTransition(.numericText(value: fraction))
            } else {
                Image(systemName: "book.closed")
                    .font(.title3)
                    .foregroundStyle(.secondary)
            }
        }
        .onAppear { withAnimation(.spring(duration: 0.9, bounce: 0.15)) { animated = fraction ?? 0 } }
        .onChange(of: fraction) { _, new in
            withAnimation(.spring(duration: 0.6)) { animated = new ?? 0 }
        }
    }
}

struct FormatBadge: View {
    let format: BookFormat

    var body: some View {
        Text(format.label)
            .font(.system(size: 9, weight: .bold, design: .rounded))
            .padding(.horizontal, 5)
            .padding(.vertical, 2)
            .background(.ultraThinMaterial, in: .capsule)
            .foregroundStyle(.primary)
    }
}

/// Stylised line drawing of an e-reader used in the sidebar and empty state.
struct KindleGlyph: View {
    var screenGlow: Double = 0

    var body: some View {
        GeometryReader { proxy in
            let w = proxy.size.width
            ZStack {
                RoundedRectangle(cornerRadius: w * 0.12)
                    .fill(LinearGradient(colors: [Color(white: 0.22), Color(white: 0.08)], startPoint: .top, endPoint: .bottom))
                RoundedRectangle(cornerRadius: w * 0.03)
                    .fill(Color(red: 0.93, green: 0.92, blue: 0.88))
                    .overlay {
                        VStack(alignment: .leading, spacing: w * 0.06) {
                            ForEach(0 ..< 6, id: \.self) { line in
                                Capsule()
                                    .fill(Color.black.opacity(0.18))
                                    .frame(width: w * (line == 5 ? 0.35 : 0.62), height: max(1, w * 0.025))
                            }
                        }
                    }
                    .overlay {
                        RoundedRectangle(cornerRadius: w * 0.03)
                            .fill(Color.accentColor.opacity(screenGlow * 0.35))
                    }
                    .padding(.horizontal, w * 0.09)
                    .padding(.top, w * 0.1)
                    .padding(.bottom, w * 0.1)
            }
        }
        .aspectRatio(0.7, contentMode: .fit)
    }
}
