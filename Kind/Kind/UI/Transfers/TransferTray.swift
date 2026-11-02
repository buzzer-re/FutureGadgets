import SwiftUI

/// Floating glass capsule that summarises uploads, downloads and edits; expands into a list.
struct TransferTray: View {
    @Environment(AppModel.self) private var model
    @State private var isExpanded = false

    var body: some View {
        let transfers = model.transfers.transfers
        if !transfers.isEmpty {
            VStack(spacing: 0) {
                if isExpanded {
                    VStack(spacing: 10) {
                        ForEach(transfers) { transfer in
                            TransferRow(transfer: transfer)
                                .transition(.move(edge: .bottom).combined(with: .opacity))
                        }
                    }
                    .padding(14)
                    Divider()
                }
                summary(transfers)
            }
            .frame(width: isExpanded ? 420 : nil)
            .glassEffect(.regular, in: .rect(cornerRadius: isExpanded ? 18 : 24))
            .shadow(color: .black.opacity(0.18), radius: 20, y: 8)
            .transition(.move(edge: .bottom).combined(with: .opacity))
            .animation(.spring(duration: 0.4, bounce: 0.2), value: isExpanded)
            .animation(.spring(duration: 0.4), value: transfers.count)
        }
    }

    private func summary(_ transfers: [Transfer]) -> some View {
        let active = model.transfers.active
        let failed = transfers.filter { if case .failed = $0.state { return true } else { return false } }
        let current = transfers.first { $0.state == .running }

        return Button {
            isExpanded.toggle()
        } label: {
            HStack(spacing: 12) {
                ZStack {
                    Circle().stroke(.quaternary, lineWidth: 3)
                    Circle()
                        .trim(from: 0, to: model.transfers.overallFraction)
                        .stroke(Color.accentColor, style: StrokeStyle(lineWidth: 3, lineCap: .round))
                        .rotationEffect(.degrees(-90))
                        .animation(.smooth, value: model.transfers.overallFraction)
                    Image(systemName: active.isEmpty ? (failed.isEmpty ? "checkmark" : "exclamationmark") : "arrow.up.arrow.down")
                        .font(.system(size: 10, weight: .bold))
                        .foregroundStyle(failed.isEmpty ? Color.accentColor : .orange)
                        .contentTransition(.symbolEffect(.replace))
                }
                .frame(width: 24, height: 24)

                VStack(alignment: .leading, spacing: 1) {
                    if let current {
                        Text("\(current.verb) · \(current.name)").lineLimit(1)
                    } else if !failed.isEmpty {
                        Text("\(failed.count) \(failed.count == 1 ? "task" : "tasks") failed")
                    } else {
                        Text("All done")
                    }
                    if active.count > 1 {
                        Text("\(active.count - 1) more waiting").font(.caption).foregroundStyle(.secondary)
                    }
                }
                .font(.callout.weight(.medium))
                .frame(maxWidth: 300, alignment: .leading)

                Image(systemName: "chevron.up")
                    .rotationEffect(.degrees(isExpanded ? 180 : 0))
                    .foregroundStyle(.secondary)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 10)
            .contentShape(.rect)
        }
        .buttonStyle(.plain)
    }
}

private struct TransferRow: View {
    @Environment(AppModel.self) private var model
    let transfer: Transfer

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: transfer.symbol)
                .font(.title3)
                .foregroundStyle(tint)
            VStack(alignment: .leading, spacing: 4) {
                Text(transfer.name).font(.callout.weight(.medium)).lineLimit(1)
                switch transfer.state {
                case .queued:
                    Text("Waiting…").font(.caption).foregroundStyle(.secondary)
                case .running:
                    ProgressView(value: transfer.fraction).controlSize(.small)
                case .finished:
                    Text("Done").font(.caption).foregroundStyle(.secondary)
                case let .failed(message):
                    Text(message).font(.caption).foregroundStyle(.orange).lineLimit(2)
                }
            }
            Spacer(minLength: 0)
            if transfer.state != .running {
                Button { model.transfers.dismiss(transfer) } label: {
                    Image(systemName: "xmark.circle.fill").foregroundStyle(.tertiary)
                }
                .buttonStyle(.plain)
            }
        }
    }

    private var tint: Color {
        switch transfer.state {
        case .failed: .orange
        case .finished: .green
        default: .accentColor
        }
    }
}
