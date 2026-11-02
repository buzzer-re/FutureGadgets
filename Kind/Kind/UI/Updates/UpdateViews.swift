import AppKit
import SwiftUI

/// Glass card pinned to the bottom of the sidebar while a newer release is available.
struct UpdateBanner: View {
    @Environment(AppModel.self) private var model
    let release: Release

    var body: some View {
        let download = model.updates.download
        Button {
            model.updates.isShowingDetails = true
        } label: {
            HStack(spacing: 10) {
                Image(systemName: symbol(for: download))
                    .font(.title2)
                    .foregroundStyle(.tint)
                    .contentTransition(.symbolEffect(.replace))
                    .symbolEffect(.bounce, value: release.tag)
                VStack(alignment: .leading, spacing: 3) {
                    Text("Kind \(release.displayVersion) is available")
                        .font(.callout.weight(.semibold))
                        .lineLimit(1)
                    Text(detail(for: download))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .contentTransition(.numericText())
                    if case let .downloading(fraction) = download {
                        ProgressView(value: fraction)
                            .controlSize(.mini)
                            .animation(.smooth, value: fraction)
                    }
                }
                Spacer(minLength: 0)
                Image(systemName: "chevron.right")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.tertiary)
            }
            .padding(12)
            .contentShape(.rect)
        }
        .buttonStyle(.plain)
        .glassEffect(.regular.tint(Color.accentColor.opacity(0.14)).interactive(), in: .rect(cornerRadius: 12))
        .padding(10)
        .help("See what's new in Kind \(release.displayVersion)")
    }

    private func symbol(for download: UpdateChecker.Download) -> String {
        switch download {
        case .ready: "checkmark.circle.fill"
        case .failed: "exclamationmark.circle.fill"
        default: "arrow.down.circle.fill"
        }
    }

    private func detail(for download: UpdateChecker.Download) -> String {
        switch download {
        case .idle: "See what's new and download it"
        case let .downloading(fraction?): "Downloading… \(Int(fraction * 100))%"
        case .downloading: "Downloading…"
        case .verifying: "Checking the download…"
        case .ready: "Downloaded. Ready to install"
        case .failed: "Download failed. Click to retry"
        }
    }
}

/// What's new in a release, plus the download → install steps.
struct UpdateSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let release: Release

    var body: some View {
        let updates = model.updates
        VStack(alignment: .leading, spacing: 0) {
            HStack(alignment: .center, spacing: 16) {
                Image(nsImage: NSApp.applicationIconImage)
                    .resizable()
                    .frame(width: 64, height: 64)
                VStack(alignment: .leading, spacing: 4) {
                    Text("A new version of Kind is available")
                        .font(.title2.weight(.semibold))
                        .fontDesign(.serif)
                    Text(subtitle(current: updates.currentVersion))
                        .foregroundStyle(.secondary)
                }
            }
            .padding(24)

            ScrollView {
                Text(ReleaseNotes.render(release.notes))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(14)
            }
            .frame(height: 220)
            .background(.quaternary.opacity(0.5), in: .rect(cornerRadius: 10))
            .padding(.horizontal, 24)

            DownloadStatus(release: release, download: updates.download)
                .padding(.horizontal, 24)
                .padding(.vertical, 16)
                .animation(.smooth, value: updates.download)

            Divider()

            HStack {
                Button("Skip This Version") { updates.skip(release) }
                    .disabled(updates.download.isActive)
                Spacer()
                if release.diskImage != nil {
                    Button("View on GitHub") { NSWorkspace.shared.open(release.pageURL) }
                }
                Button("Later") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                primaryButton(updates)
                    .keyboardShortcut(.defaultAction)
            }
            .padding(16)
        }
        .frame(width: 540)
    }

    @ViewBuilder
    private func primaryButton(_ updates: UpdateChecker) -> some View {
        switch updates.download {
        case _ where release.diskImage == nil:
            Button("Open Release Page") { NSWorkspace.shared.open(release.pageURL) }
        case .idle:
            Button("Download Update") { updates.downloadUpdate() }
        case .failed:
            Button("Try Again") { updates.downloadUpdate() }
        case .downloading, .verifying:
            Button("Cancel Download") { updates.cancelDownload() }
        case .ready:
            Button("Quit Kind") { NSApp.terminate(nil) }
                .disabled(!model.transfers.active.isEmpty)
                .help(model.transfers.active.isEmpty ? "" : "Wait for the Kindle transfers to finish first.")
        }
    }

    private func subtitle(current: AppVersion?) -> String {
        var parts = ["Kind \(release.displayVersion)"]
        if let current { parts.append("You have \(current)") }
        if let date = release.publishedAt { parts.append("Released \(date.formatted(date: .abbreviated, time: .omitted))") }
        return parts.joined(separator: " · ")
    }
}

private struct DownloadStatus: View {
    let release: Release
    let download: UpdateChecker.Download

    var body: some View {
        switch download {
        case .idle:
            if let dmg = release.diskImage {
                Label("\(dmg.name) · \(dmg.size.byteString). It's saved to your Downloads folder and checked against the release's checksum.",
                      systemImage: "externaldrive")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            } else {
                Label("This release has no disk image attached. Download it from the release page.", systemImage: "info.circle")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
        case let .downloading(fraction):
            ProgressView(value: fraction) {
                Text("Downloading \(release.diskImage?.name ?? "update")…")
            } currentValueLabel: {
                if let fraction { Text(fraction, format: .percent.precision(.fractionLength(0))).monospacedDigit() }
            }
        case .verifying:
            ProgressView {
                Text("Checking the download…")
            }
            .progressViewStyle(.linear)
        case let .ready(url):
            VStack(alignment: .leading, spacing: 8) {
                Label("Downloaded and verified", systemImage: "checkmark.seal.fill")
                    .font(.callout.weight(.semibold))
                    .foregroundStyle(.green)
                Text("The disk image is open in Finder. Quit Kind, drag the new Kind onto Applications and choose **Replace**, then open it again.")
                    .font(.callout)
                    .fixedSize(horizontal: false, vertical: true)
                HStack(spacing: 12) {
                    Button("Open Disk Image") { NSWorkspace.shared.open(url) }
                    Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([url]) }
                }
                .buttonStyle(.link)
                .font(.callout)
            }
        case let .failed(message):
            Label(message, systemImage: "exclamationmark.triangle.fill")
                .font(.callout)
                .foregroundStyle(.orange)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}

/// Renders GitHub release notes. SwiftUI `Text` only draws inline Markdown, so block syntax
/// (headings, bullets) is handled line by line here.
nonisolated enum ReleaseNotes {
    static func render(_ markdown: String?) -> AttributedString {
        let text = (markdown ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return AttributedString("No release notes.") }

        var result = AttributedString()
        var isFirstLine = true
        for rawLine in text.components(separatedBy: .newlines) {
            var line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("<!--") { continue }
            var isHeading = false
            if line.hasPrefix("#") {
                line = String(line.drop { $0 == "#" }).trimmingCharacters(in: .whitespaces)
                isHeading = true
            } else if line.hasPrefix("- ") || line.hasPrefix("* ") {
                line = "•  " + line.dropFirst(2)
            }
            var rendered = (try? AttributedString(markdown: line, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace)))
                ?? AttributedString(line)
            if isHeading { rendered.font = .headline }
            if !isFirstLine { result += AttributedString("\n") }
            result += rendered
            isFirstLine = false
        }
        return result
    }
}

#if DEBUG
extension PreviewSupport {
    static let release = Release(
        tag: "v1.2.0", name: "Kind 1.2.0",
        notes: """
        ## Installing

        1. Open the DMG and drag **Kind** to **Applications**.
        2. Choose *Replace* when asked.

        ## What's Changed
        * Faster library scans over USB
        * Edit covers for PDFs
        """,
        pageURL: URL(string: "https://github.com")!, publishedAt: .now.addingTimeInterval(-86_400),
        assets: [.init(name: "Kind-1.2.0.dmg", size: 8_400_000, downloadURL: URL(string: "https://github.com")!)])

    static func updateModel(_ download: UpdateChecker.Download = .idle) -> AppModel {
        let model = model()
        model.updates.latest = release
        model.updates.previewDownload(download)
        return model
    }
}

#Preview("Update Sheet") {
    UpdateSheet(release: PreviewSupport.release)
        .environment(PreviewSupport.updateModel())
}

#Preview("Update Ready") {
    UpdateSheet(release: PreviewSupport.release)
        .environment(PreviewSupport.updateModel(.ready(URL(filePath: "/tmp/Kind-1.2.0.dmg"))))
}

#Preview("Update Banner") {
    RootView()
        .environment(PreviewSupport.updateModel(.downloading(fraction: 0.42)))
        .frame(width: 1240, height: 780)
}
#endif
