import AppKit
import Combine
import Foundation
import UniformTypeIdentifiers

struct EngineStatus: Codable, Equatable {
    var winePath: String?
    var wineOk: Bool
    var winetricksPath: String?
    var prefix: String?
    var prefixOk: Bool
    var directMusic: Bool
    var gameDir: String?
    var baseExe: String?
    var aocExe: String?
    var musicEnabled: Bool
    var virtualDesktop: Bool
    var virtualDesktopSize: String
    var graphicsStack: String
    var canPlay: Bool
    var canPlayAoc: Bool
    var errors: [String]
    var warnings: [String]

    static let empty = EngineStatus(
        winePath: nil,
        wineOk: false,
        winetricksPath: nil,
        prefix: nil,
        prefixOk: false,
        directMusic: false,
        gameDir: nil,
        baseExe: nil,
        aocExe: nil,
        musicEnabled: false,
        virtualDesktop: false,
        virtualDesktopSize: "1920x1080",
        graphicsStack: "d7vk",
        canPlay: false,
        canPlayAoc: false,
        errors: [],
        warnings: []
    )
}

@MainActor
final class LauncherModel: ObservableObject {
    @Published var status: EngineStatus = .empty
    @Published var isBusy = false
    @Published var lastMessage = "Attach your GOG copy of Empire Earth to play on this Mac."
    @Published var lastError: String?

    private let decoder: JSONDecoder = {
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        return decoder
    }()

    func refresh() async {
        do {
            let result = try await runScript("status.sh")
            if let data = result.stdout.data(using: .utf8) {
                status = try decoder.decode(EngineStatus.self, from: data)
                lastError = nil
            }
        } catch {
            lastError = error.localizedDescription
        }
    }

    func installWine() async {
        await runLong("Downloading free Wine stable…", script: "install-wine.sh")
    }

    func setupPrefix() async {
        await runLong("Setting up the Wine prefix and DirectMusic. This can take several minutes.", script: "setup-prefix.sh")
    }

    func chooseGame() async {
        let panel = NSOpenPanel()
        panel.title = "Choose Empire Earth"
        panel.prompt = "Choose"
        panel.canChooseFiles = true
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        if let exeType = UTType(filenameExtension: "exe") {
            panel.allowedContentTypes = [exeType]
        }
        guard panel.runModal() == .OK, let url = panel.url else { return }

        let path = url.path
        let name = url.lastPathComponent.lowercased()
        if name.hasPrefix("setup") || name.contains("install") {
            await runLong("Installing Empire Earth into the Wine prefix…", script: "install-game.sh", arguments: [path])
            return
        }
        await runLong("Saving game folder…", script: "set-game.sh", arguments: [path])
    }

    func play(mode: String) async {
        await runLong(mode == "aoc" ? "Launching Art of Conquest (stops if still on the banner after 60s)…" : "Launching Empire Earth (stops if still on the banner after 60s)…", script: "launch.sh", arguments: [mode])
    }

    func setMusic(_ enabled: Bool) async {
        await runLong(nil, script: "set-options.sh", arguments: ["--music", enabled ? "1" : "0"])
    }

    func setVirtualDesktop(_ enabled: Bool) async {
        await runLong(nil, script: "set-options.sh", arguments: ["--virtual-desktop", enabled ? "1" : "0"])
    }

    func setGraphics(_ stack: String) async {
        await runLong(nil, script: "set-options.sh", arguments: ["--graphics", stack])
    }

    func openLogs() {
        let logs = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/EmpireEarthMac/logs")
        try? FileManager.default.createDirectory(at: logs, withIntermediateDirectories: true)
        NSWorkspace.shared.open(logs)
    }

    private func runLong(_ message: String?, script: String, arguments: [String] = []) async {
        isBusy = true
        lastError = nil
        if let message {
            lastMessage = message
        }
        do {
            let result = try await runScript(script, arguments: arguments)
            if result.status != 0 {
                lastError = cleaned(result.stderr.isEmpty ? result.stdout : result.stderr)
            } else if !result.stdout.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                lastMessage = result.stdout.trimmingCharacters(in: .whitespacesAndNewlines)
            } else if message != nil {
                lastMessage = "Done."
            }
        } catch {
            lastError = error.localizedDescription
        }
        await refresh()
        isBusy = false
    }

    private func cleaned(_ text: String) -> String {
        let lines = text.split(separator: "\n").map(String.init)
        if let errorLine = lines.last(where: { $0.lowercased().contains("error:") }) {
            return errorLine.replacingOccurrences(of: "error: ", with: "")
        }
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.count > 400 {
            return String(trimmed.suffix(400))
        }
        return trimmed
    }

    private func runScript(_ name: String, arguments: [String] = []) async throws -> ScriptResult {
        guard let script = scriptsDirectory()?.appendingPathComponent(name) else {
            throw LauncherError.scriptsMissing
        }
        return try await Task.detached(priority: .userInitiated) {
            try ScriptRunner.run(script: script, arguments: arguments)
        }.value
    }

    private func scriptsDirectory() -> URL? {
        let bundled = Bundle.main.resourceURL?.appendingPathComponent("scripts")
        if let bundled, FileManager.default.isExecutableFile(atPath: bundled.appendingPathComponent("status.sh").path) {
            return bundled
        }
        var url = Bundle.main.bundleURL
        for _ in 0..<8 {
            let candidate = url.appendingPathComponent("scripts")
            if FileManager.default.isExecutableFile(atPath: candidate.appendingPathComponent("status.sh").path) {
                return candidate
            }
            url.deleteLastPathComponent()
        }
        return nil
    }
}

struct ScriptResult: Sendable {
    var status: Int32
    var stdout: String
    var stderr: String
}

enum LauncherError: LocalizedError {
    case scriptsMissing

    var errorDescription: String? {
        switch self {
        case .scriptsMissing:
            return "Launcher scripts were not found. Rebuild with scripts/build-app.sh from the project folder."
        }
    }
}

enum ScriptRunner {
    static func run(script: URL, arguments: [String]) throws -> ScriptResult {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/bash")
        process.arguments = [script.path] + arguments
        process.environment = ProcessInfo.processInfo.environment
        let stdout = Pipe()
        let stderr = Pipe()
        process.standardOutput = stdout
        process.standardError = stderr
        try process.run()
        process.waitUntilExit()
        let out = String(data: stdout.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        let err = String(data: stderr.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        return ScriptResult(status: process.terminationStatus, stdout: out, stderr: err)
    }
}
