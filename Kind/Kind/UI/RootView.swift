import SwiftUI
import UniformTypeIdentifiers

struct RootView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model
        @Bindable var updates = model.updates
        NavigationSplitView {
            SidebarView()
                .navigationSplitViewColumnWidth(min: 220, ideal: 250, max: 320)
        } detail: {
            Group {
                if let device = model.selectedDevice {
                    LibraryView(device: device)
                        .id(device.id)
                        .transition(.opacity.combined(with: .scale(scale: 0.98)))
                } else {
                    ConnectKindleView(isConnecting: model.devices.isConnectingUSB)
                        .transition(.opacity)
                }
            }
            .animation(.smooth(duration: 0.35), value: model.selectedDevice?.id)
        }
        .inspector(isPresented: Binding(
            get: { model.showInspector && model.selectedDevice != nil },
            set: { model.showInspector = $0 }
        )) {
            InspectorContainer()
                .inspectorColumnWidth(min: 280, ideal: 320, max: 420)
        }
        .overlay(alignment: .bottom) {
            TransferTray()
                .padding(.bottom, 16)
        }
        .fileImporter(isPresented: $model.isImporting,
                      allowedContentTypes: BookFormat.importTypes,
                      allowsMultipleSelection: true) { result in
            if case let .success(urls) = result { model.actions?.upload(urls) }
        }
        .sheet(item: $model.editingBook) { book in
            if let device = model.selectedDevice {
                MetadataEditor(book: book, device: device)
            }
        }
        .confirmationDialog(deleteTitle, isPresented: Binding(
            get: { !model.pendingDeletion.isEmpty },
            set: { if !$0 { model.pendingDeletion = [] } }
        ), titleVisibility: .visible) {
            Button("Delete from Kindle", role: .destructive) { model.confirmDelete() }
        } message: {
            Text("Reading progress, highlights and notes stored with the book on the Kindle are removed too. This can't be undone.")
        }
        .alert("Kindle Error", isPresented: Binding(
            get: { model.devices.lastError != nil },
            set: { if !$0 { model.devices.lastError = nil } }
        )) {
            Button("OK") {}
        } message: {
            Text(model.devices.lastError ?? "")
        }
        .sheet(isPresented: $updates.isShowingDetails) {
            if let release = updates.latest {
                UpdateSheet(release: release)
            }
        }
        .alert(updateNoticeTitle, isPresented: Binding(
            get: { updates.notice != nil },
            set: { if !$0 { updates.notice = nil } }
        )) {
            Button("OK") {}
        } message: {
            Text(updateNoticeMessage)
        }
    }

    private var updateNoticeTitle: String {
        if case .failed = model.updates.notice { return "Couldn't Check for Updates" }
        return "You're Up to Date"
    }

    private var updateNoticeMessage: String {
        switch model.updates.notice {
        case let .upToDate(version): "Kind \(version) is the newest version available."
        case let .failed(message): message
        case nil: ""
        }
    }

    private var deleteTitle: String {
        model.pendingDeletion.count == 1
            ? "Delete “\(model.pendingDeletion[0].title)”?"
            : "Delete \(model.pendingDeletion.count) books?"
    }
}

nonisolated extension BookFormat {
    /// Types offered in the "Add Books" panel: every recognised book extension.
    static var importTypes: [UTType] {
        [.pdf, .epub, .plainText] + fileExtensions.compactMap { UTType(filenameExtension: $0, conformingTo: .data) }
    }
}
