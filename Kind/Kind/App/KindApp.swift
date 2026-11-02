import SwiftUI

@main
struct KindApp: App {
    @State private var model = AppModel()

    var body: some Scene {
        WindowGroup("Kind", id: "library") {
            RootView()
                .environment(model)
                .frame(minWidth: 880, minHeight: 560)
                .task { model.start() }
        }
        .windowToolbarStyle(.unified)
        .defaultSize(width: 1240, height: 780)
        .commands { KindCommands(model: model) }
    }
}

struct KindCommands: Commands {
    let model: AppModel

    var body: some Commands {
        CommandGroup(after: .appInfo) {
            Button("Check for Updates…") {
                Task { await model.updates.check(userInitiated: true) }
            }
            .disabled(model.updates.isChecking)
        }
        CommandGroup(after: .newItem) {
            Button("Add Books to Kindle…") { model.isImporting = true }
                .keyboardShortcut("o")
                .disabled(model.selectedDevice == nil)
        }
        CommandMenu("Library") {
            Button("Refresh") { model.selectedDevice?.refresh() }
                .keyboardShortcut("r")
                .disabled(model.selectedDevice == nil)
            Button("Edit Book Info…") { model.editSelected() }
                .keyboardShortcut("i")
                .disabled(model.selectedBooks.count != 1)
            Divider()
            Picker("View As", selection: Binding(get: { model.viewMode }, set: { model.viewMode = $0 })) {
                ForEach(AppModel.ViewMode.allCases) { mode in
                    Label(mode.title, systemImage: mode.symbol).tag(mode)
                }
            }
            Divider()
            Button("Eject Kindle") {
                if let device = model.selectedDevice { Task { await model.devices.eject(device) } }
            }
            .keyboardShortcut("e")
            .disabled(model.selectedDevice == nil)
        }
    }
}
