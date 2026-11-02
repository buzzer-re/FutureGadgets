import SwiftUI

struct SidebarView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model
        List {
            Section("Devices") {
                if model.devices.devices.isEmpty {
                    WaitingRow(isConnecting: model.devices.isConnectingUSB)
                }
                ForEach(model.devices.devices) { device in
                    DeviceRow(device: device, isSelected: model.selectedDevice?.id == device.id)
                        .contentShape(.rect)
                        .onTapGesture { model.selectedDeviceID = device.id }
                        .transition(.move(edge: .leading).combined(with: .opacity))
                }
            }

            if model.selectedDevice != nil {
                Section("Library") {
                    ForEach(LibraryFilter.allCases) { filter in
                        FilterRow(filter: filter, count: model.count(for: filter), isSelected: model.filter == filter)
                            .contentShape(.rect)
                            .onTapGesture {
                                withAnimation(.snappy) { model.filter = filter }
                            }
                    }
                }
            }
        }
        .listStyle(.sidebar)
        .animation(.smooth, value: model.devices.devices.count)
        .safeAreaInset(edge: .bottom) {
            if let release = model.updates.announced {
                UpdateBanner(release: release)
                    .transition(.move(edge: .bottom).combined(with: .opacity))
            }
        }
        .animation(.spring(duration: 0.45, bounce: 0.2), value: model.updates.announced?.tag)
    }
}

private struct FilterRow: View {
    let filter: LibraryFilter
    let count: Int
    let isSelected: Bool

    var body: some View {
        HStack {
            Label(filter.title, systemImage: filter.symbol)
                .symbolVariant(isSelected ? .fill : .none)
            Spacer()
            if count > 0 {
                Text(count, format: .number)
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
                    .contentTransition(.numericText(value: Double(count)))
            }
        }
        .padding(.vertical, 3)
        .padding(.horizontal, 6)
        .background {
            if isSelected {
                RoundedRectangle(cornerRadius: 6).fill(Color.accentColor.opacity(0.18))
            }
        }
        .foregroundStyle(isSelected ? Color.accentColor : Color.primary)
    }
}

private struct DeviceRow: View {
    @Environment(AppModel.self) private var model
    let device: KindleDevice
    let isSelected: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 10) {
                KindleGlyph(screenGlow: device.phase.isBusy ? 1 : 0)
                    .frame(width: 24)
                    .phaseAnimator([0.4, 1.0], trigger: device.phase.isBusy) { view, phase in
                        view.opacity(device.phase.isBusy ? phase : 1)
                    }
                VStack(alignment: .leading, spacing: 1) {
                    Text(device.name).font(.headline).lineLimit(1)
                    Text(device.model)
                    .help("Connected via \(device.connection.label)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                }
                Spacer(minLength: 0)
                if let battery = device.batteryLevel {
                    BatteryIndicator(level: battery)
                }
                Button {
                    Task { await model.devices.eject(device) }
                } label: {
                    Image(systemName: "eject.fill")
                }
                .buttonStyle(.borderless)
                .foregroundStyle(.secondary)
                .help("Eject \(device.name)")
            }
            if let storage = device.storage {
                StorageBar(storage: storage)
            }
            PhaseLabel(phase: device.phase)
            if case .failed = device.phase {
                Button("Retry") { model.devices.reconnect(device) }
                    .controlSize(.small)
            }
        }
        .padding(10)
        .background {
            RoundedRectangle(cornerRadius: 10)
                .fill(isSelected ? AnyShapeStyle(.selection.opacity(0.35)) : AnyShapeStyle(.quaternary.opacity(0.5)))
        }
        .listRowInsets(EdgeInsets(top: 4, leading: 4, bottom: 4, trailing: 4))
        .contextMenu {
            Button("Refresh Library") { model.devices.reconnect(device) }
            if let url = device.volumeURL {
                Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([url]) }
            }
            Divider()
            Button("Eject") { Task { await model.devices.eject(device) } }
        }
    }
}

private struct PhaseLabel: View {
    let phase: KindleDevice.Phase

    var body: some View {
        switch phase {
        case .connecting:
            Label("Connecting…", systemImage: "cable.connector").font(.caption2).foregroundStyle(.secondary)
        case .listing:
            Label("Reading library…", systemImage: "books.vertical").font(.caption2).foregroundStyle(.secondary)
        case let .enriching(done, total) where done < total:
            ProgressView(value: Double(done), total: Double(max(total, 1))) {
                Text("Indexing \(done) of \(total)").font(.caption2).foregroundStyle(.secondary)
            }
            .controlSize(.small)
        case let .failed(message):
            Label(message, systemImage: "exclamationmark.triangle.fill")
                .font(.caption2)
                .foregroundStyle(.orange)
                .lineLimit(3)
        default:
            EmptyView()
        }
    }
}

struct StorageBar: View {
    let storage: StorageInfo

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            GeometryReader { proxy in
                ZStack(alignment: .leading) {
                    Capsule().fill(.quaternary)
                    Capsule()
                        .fill(LinearGradient(colors: [.accentColor.opacity(0.7), .accentColor], startPoint: .leading, endPoint: .trailing))
                        .frame(width: max(4, proxy.size.width * storage.usedFraction))
                }
            }
            .frame(height: 5)
            Text("\(storage.free.byteString) free of \(storage.capacity.byteString)")
                .font(.caption2)
                .foregroundStyle(.secondary)
                .monospacedDigit()
        }
        .animation(.smooth, value: storage)
    }
}

private struct BatteryIndicator: View {
    let level: Int

    var body: some View {
        let symbol = switch level {
        case ..<13: "battery.0percent"
        case ..<38: "battery.25percent"
        case ..<63: "battery.50percent"
        case ..<88: "battery.75percent"
        default: "battery.100percent"
        }
        Image(systemName: symbol)
            .foregroundStyle(level < 15 ? .red : .secondary)
            .help("Battery \(level)%")
    }
}

private struct WaitingRow: View {
    let isConnecting: Bool
    @State private var pulse = false

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: "cable.connector")
                .symbolEffect(.pulse, options: .repeating, isActive: true)
                .foregroundStyle(.secondary)
            Text(isConnecting ? "Connecting to Kindle…" : "Waiting for a Kindle")
                .foregroundStyle(.secondary)
        }
        .font(.callout)
        .padding(.vertical, 6)
    }
}
