import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var model: LauncherModel

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            header
            statusCard
            gameCard
            optionsCard
            actions
            footer
        }
        .padding(24)
        .frame(width: 620)
        .background(Color(nsColor: .windowBackgroundColor))
        .task { await model.refresh() }
        .disabled(model.isBusy)
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Empire Earth")
                .font(.largeTitle.weight(.semibold))
            Text("Play your GOG copy on this Mac. Wine is free — no extra purchase.")
                .foregroundStyle(.secondary)
        }
    }

    private var statusCard: some View {
        GroupBox("Runtime") {
            VStack(alignment: .leading, spacing: 10) {
                statusRow("Wine", ok: model.status.wineOk, detail: model.status.winePath ?? "Not installed")
                statusRow("Prefix", ok: model.status.prefixOk, detail: model.status.prefixOk ? "Ready (DirectMusic installed)" : "Needs setup")
                HStack {
                    Button("Install Wine") {
                        Task { await model.installWine() }
                    }
                    Button("Set Up Prefix") {
                        Task { await model.setupPrefix() }
                    }
                    Spacer()
                }
            }
            .padding(.top, 4)
        }
    }

    private var gameCard: some View {
        GroupBox("Game") {
            VStack(alignment: .leading, spacing: 10) {
                LabeledContent("Folder") {
                    Text(model.status.gameDir ?? "Not selected")
                        .lineLimit(2)
                        .foregroundStyle(model.status.gameDir == nil ? .secondary : .primary)
                }
                LabeledContent("Empire Earth") {
                    Text(model.status.baseExe == nil ? "Missing" : "Found")
                        .foregroundStyle(model.status.baseExe == nil ? Color.orange : Color.green)
                }
                LabeledContent("Art of Conquest") {
                    Text(model.status.aocExe == nil ? "Not found" : "Found")
                        .foregroundStyle(model.status.aocExe == nil ? Color.secondary : Color.green)
                }
                HStack {
                    Button("Choose GOG Folder or Setup") {
                        Task { await model.chooseGame() }
                    }
                    Spacer()
                }
            }
            .padding(.top, 4)
        }
    }

    private var optionsCard: some View {
        GroupBox("Options") {
            VStack(alignment: .leading, spacing: 8) {
                Toggle("In-game music (same as the game's Options > Music Quality)", isOn: musicBinding)
                Toggle("Virtual desktop (only used when full screen is off)", isOn: desktopBinding)
                Picker("Graphics", selection: graphicsBinding) {
                    Text("D7VK + DXVK (default)").tag("d7vk")
                    Text("dgVoodoo 2.79 + DXVK (does not reach the menu)").tag("dgvoodoo")
                    Text("GOG D3D7→D3D9 + DXVK (does not reach the menu)").tag("gog-d3d9")
                    Text("dgVoodoo + Wine wined3d (does not reach the menu)").tag("dgvoodoo-wined3d")
                    Text("dgVoodoo + DXMT (does not reach the menu)").tag("dxmt")
                    Text("D3DMetal / GPTK (needs 32-bit DLLs)").tag("d3dmetal")
                }
            }
            .padding(.top, 4)
        }
    }

    private var actions: some View {
        HStack {
            Button {
                Task { await model.play(mode: "base") }
            } label: {
                Text("Play")
                    .frame(minWidth: 96)
            }
            .keyboardShortcut(.defaultAction)
            .disabled(!model.status.canPlay)

            Button {
                Task { await model.play(mode: "aoc") }
            } label: {
                Text("Play Art of Conquest")
            }
            .disabled(!model.status.canPlayAoc)

            Spacer()
            Button("Open Logs") { model.openLogs() }
        }
    }

    private var footer: some View {
        VStack(alignment: .leading, spacing: 8) {
            if model.isBusy {
                ProgressView(model.lastMessage)
            } else {
                Text(model.lastMessage)
                    .foregroundStyle(.secondary)
            }
            if let error = model.lastError, !error.isEmpty {
                Text(error)
                    .foregroundStyle(.red)
            }
            ForEach(model.status.warnings, id: \.self) { warning in
                Text(warning)
                    .font(.caption)
                    .foregroundStyle(.orange)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func statusRow(_ title: String, ok: Bool, detail: String) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Circle()
                .fill(ok ? Color.green : Color.orange)
                .frame(width: 8, height: 8)
            Text(title)
                .fontWeight(.medium)
            Spacer()
            Text(detail)
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .truncationMode(.middle)
        }
    }

    private var musicBinding: Binding<Bool> {
        Binding(
            get: { model.status.musicEnabled },
            set: { value in
                Task { await model.setMusic(value) }
            }
        )
    }

    private var desktopBinding: Binding<Bool> {
        Binding(
            get: { model.status.virtualDesktop },
            set: { value in
                Task { await model.setVirtualDesktop(value) }
            }
        )
    }

    private var graphicsBinding: Binding<String> {
        Binding(
            get: { model.status.graphicsStack },
            set: { value in
                Task { await model.setGraphics(value) }
            }
        )
    }
}
