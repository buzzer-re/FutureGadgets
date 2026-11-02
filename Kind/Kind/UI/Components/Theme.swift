import SwiftUI

enum Theme {
    static let coverAspect: CGFloat = 2 / 3
    static let coverCorner: CGFloat = 6
    static let cardCorner: CGFloat = 14

    /// Warm, paper-inspired palette used for generated covers. Picked per book from a stable hash.
    static let coverPalettes: [(Color, Color)] = [
        (Color(red: 0.16, green: 0.23, blue: 0.35), Color(red: 0.36, green: 0.47, blue: 0.62)),
        (Color(red: 0.42, green: 0.15, blue: 0.17), Color(red: 0.72, green: 0.34, blue: 0.29)),
        (Color(red: 0.13, green: 0.30, blue: 0.27), Color(red: 0.36, green: 0.58, blue: 0.49)),
        (Color(red: 0.29, green: 0.20, blue: 0.40), Color(red: 0.55, green: 0.42, blue: 0.67)),
        (Color(red: 0.48, green: 0.33, blue: 0.13), Color(red: 0.82, green: 0.62, blue: 0.32)),
        (Color(red: 0.20, green: 0.20, blue: 0.22), Color(red: 0.45, green: 0.45, blue: 0.48)),
        (Color(red: 0.08, green: 0.29, blue: 0.40), Color(red: 0.25, green: 0.58, blue: 0.66)),
    ]

    static func palette(for seed: String) -> (Color, Color) {
        // FNV-1a: stable across launches, unlike `hashValue`.
        var hash: UInt64 = 0xcbf2_9ce4_8422_2325
        for byte in seed.utf8 { hash = (hash ^ UInt64(byte)) &* 0x100_0000_01b3 }
        return coverPalettes[Int(hash % UInt64(coverPalettes.count))]
    }
}

extension ReadingProgress.State {
    var label: String {
        switch self {
        case .unread: "Unread"
        case .reading: "Reading"
        case .finished: "Finished"
        }
    }

    var tint: Color {
        switch self {
        case .unread: .secondary
        case .reading: .accentColor
        case .finished: .green
        }
    }
}

extension Int {
    var byteString: String { ByteCountFormatter.string(fromByteCount: Int64(self), countStyle: .file) }
}

extension Int64 {
    var byteString: String { ByteCountFormatter.string(fromByteCount: self, countStyle: .file) }
}
