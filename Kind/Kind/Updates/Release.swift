import Foundation

/// A dotted numeric version such as `1.2.3`. Missing components count as zero, so `1.2 == 1.2.0`.
nonisolated struct AppVersion: Comparable, Hashable, CustomStringConvertible, Sendable {
    let components: [Int]

    /// Accepts `1.2.3` and tag spellings like `v1.2.3`. Pre-release suffixes (`-beta`) aren't
    /// supported: the release workflow only publishes plain numeric tags.
    init?(_ string: String) {
        var text = Substring(string.trimmingCharacters(in: .whitespaces))
        if text.first == "v" || text.first == "V" { text = text.dropFirst() }
        let parts = text.split(separator: ".", omittingEmptySubsequences: false)
        guard (1 ... 4).contains(parts.count) else { return nil }
        var numbers: [Int] = []
        for part in parts {
            guard !part.isEmpty, part.allSatisfy({ $0.isASCII && $0.isNumber }), let number = Int(part) else { return nil }
            numbers.append(number)
        }
        components = numbers
    }

    var description: String { components.map(String.init).joined(separator: ".") }

    /// Components without trailing zeros, so equal versions compare and hash the same.
    private var significant: ArraySlice<Int> {
        var slice = components[...]
        while slice.last == 0 { slice = slice.dropLast() }
        return slice
    }

    static func == (lhs: AppVersion, rhs: AppVersion) -> Bool { lhs.significant == rhs.significant }

    func hash(into hasher: inout Hasher) { hasher.combine(Array(significant)) }

    static func < (lhs: AppVersion, rhs: AppVersion) -> Bool {
        for index in 0 ..< max(lhs.components.count, rhs.components.count) {
            let left = index < lhs.components.count ? lhs.components[index] : 0
            let right = index < rhs.components.count ? rhs.components[index] : 0
            if left != right { return left < right }
        }
        return false
    }
}

/// The fields Kind uses from GitHub's "latest release" API response.
nonisolated struct Release: Decodable, Equatable, Sendable {
    nonisolated struct Asset: Decodable, Equatable, Sendable {
        let name: String
        let size: Int64
        let downloadURL: URL

        enum CodingKeys: String, CodingKey {
            case name, size
            case downloadURL = "browser_download_url"
        }
    }

    let tag: String
    let name: String?
    let notes: String?
    let pageURL: URL
    let publishedAt: Date?
    let assets: [Asset]

    enum CodingKeys: String, CodingKey {
        case tag = "tag_name"
        case name
        case notes = "body"
        case pageURL = "html_url"
        case publishedAt = "published_at"
        case assets
    }

    static func decode(_ data: Data) throws -> Release {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return try decoder.decode(Release.self, from: data)
    }

    var version: AppVersion? { AppVersion(tag) }

    /// "1.2.0", falling back to the raw tag for tags that aren't plain versions.
    var displayVersion: String { version?.description ?? tag }

    /// The DMG that `scripts/build-dmg.sh` attaches to every release.
    var diskImage: Asset? {
        assets.first { $0.name.lowercased().hasSuffix(".dmg") }
    }

    /// The `<dmg>.sha256` file published next to the disk image.
    var diskImageChecksum: Asset? {
        guard let diskImage else { return nil }
        return assets.first { $0.name == diskImage.name + ".sha256" }
    }

    /// Reads a `shasum -a 256` line (`<hex>  <file name>`) and returns the lowercase digest.
    static func parseChecksum(_ text: String) -> String? {
        guard let token = text.split(whereSeparator: \.isWhitespace).first, token.count == 64,
              token.allSatisfy(\.isHexDigit) else { return nil }
        return token.lowercased()
    }
}
