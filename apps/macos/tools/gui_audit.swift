// Compiled with the unmodified app views and view model by run_gui_audit.py.
// Exercises model actions and native NSEvents in the real AppKit/SwiftUI views.
// Events are dispatched inside this app; not OS-level HID or accessibility automation.
import Foundation

@main
@MainActor
struct GUIAudit {
    static var samples: [[String: Any]] = []
    static var heartbeatGaps: [Double] = []
    static var lastTick = ProcessInfo.processInfo.systemUptime
    static var heartbeat: Timer?
    static var workspace: WorkspaceViewModel!
    static var window: NSWindow!
    static var output: URL!
    static var failed = false

    static func main() {
        output = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
        try! FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
        let app = NSApplication.shared
        app.setActivationPolicy(.regular)
        workspace = WorkspaceViewModel()
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1240, height: 680), styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.title = "LumenCode GUI audit"
        window.contentView = NSHostingView(rootView: ContentView(workspace: workspace))
        window.center()
        window.makeKeyAndOrderFront(nil)
        app.activate(ignoringOtherApps: true)
        heartbeat = Timer(timeInterval: 0.01, repeats: true) { _ in
            MainActor.assumeIsolated {
                let now = ProcessInfo.processInfo.systemUptime
                heartbeatGaps.append((now - lastTick) * 1000)
                lastTick = now
            }
        }
        RunLoop.main.add(heartbeat!, forMode: .common)
        Task {
            await run()
            if failed { exit(1) }
            app.terminate(nil)
        }
        app.run()
    }

    static func settle() async { try? await Task.sleep(for: .milliseconds(150)) }

    static func capture(_ name: String) {
        guard ProcessInfo.processInfo.environment["LUMENCODE_AUDIT_NO_CAPTURES"] != "1" else { return }
        print("GUI capture: \(name)")
        fflush(stdout)
        guard let view = window.contentView else { return }
        view.layoutSubtreeIfNeeded()
        guard let bitmap = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return }
        view.cacheDisplay(in: view.bounds, to: bitmap)
        try? bitmap.representation(using: .png, properties: [:])?.write(to: output.appendingPathComponent(name + ".png"))
    }

    static func measure(_ name: String, native: Bool = false, action: () -> Void) async {
        heartbeatGaps = []
        lastTick = ProcessInfo.processInfo.systemUptime
        let start = lastTick
        action()
        // Native selection publishes on the next main-queue turn. Include that
        // handoff in the measured interval rather than reporting a false 0 ms.
        if native { try? await Task.sleep(for: .milliseconds(10)) }
        while (workspace.isScanning || workspace.isAnalyzingFile || workspace.isSearching) && ProcessInfo.processInfo.systemUptime - start < 30 {
            try? await Task.sleep(for: .milliseconds(10))
        }
        let ready = ProcessInfo.processInfo.systemUptime
        // Let the presentation update so heartbeat catches rendering stalls too.
        await settle()
        let gaps = heartbeatGaps.sorted()
        samples.append([
            "name": name, "data_ready_ms": (ready-start)*1000,
            "native_event": native,
            "heartbeat_max_gap_ms": gaps.last ?? 0,
            "heartbeat_p95_gap_ms": gaps.isEmpty ? 0 : gaps[min(gaps.count-1, Int(Double(gaps.count)*0.95))],
            "heartbeat_samples": gaps.count,
            "timed_out": workspace.isScanning || workspace.isAnalyzingFile || workspace.isSearching,
            "selected_file": workspace.selectedAnalysis.fileName,
            "selected_path": workspace.selectedAnalysis.path,
            "selection_matches_request": workspace.selectedEntryID == workspace.selectedAnalysis.path,
            "symbols": workspace.selectedAnalysis.symbols.count,
            "scanned_files": workspace.scanResult.projectSummary.totalFiles,
            "error": workspace.errorMessage
        ])
        print("GUI audit: \(name), \(Int((ready-start)*1000)) ms, heartbeat max \(Int(gaps.last ?? 0)) ms")
        fflush(stdout)
    }

    static func findView<T: NSView>(_ type: T.Type, matching: (T) -> Bool = { _ in true }) -> T? {
        func visit(_ view: NSView) -> T? {
            if let match = view as? T, matching(match) { return match }
            for child in view.subviews { if let match = visit(child) { return match } }
            return nil
        }
        return window.contentView.flatMap(visit)
    }

    static func key(_ code: UInt16, characters: String) {
        for type: NSEvent.EventType in [.keyDown, .keyUp] {
            if let event = NSEvent.keyEvent(with: type, location: .zero, modifierFlags: [],
                                          timestamp: ProcessInfo.processInfo.systemUptime,
                                          windowNumber: window.windowNumber, context: nil,
                                          characters: characters, charactersIgnoringModifiers: characters,
                                          isARepeat: false, keyCode: code) { NSApp.sendEvent(event) }
        }
    }

    static func click(_ view: NSView, at point: NSPoint? = nil) {
        let point = view.convert(point ?? NSPoint(x: view.bounds.midX, y: view.bounds.midY), to: nil)
        for type: NSEvent.EventType in [.leftMouseDown, .leftMouseUp] {
            if let event = NSEvent.mouseEvent(with: type, location: point, modifierFlags: [],
                                            timestamp: ProcessInfo.processInfo.systemUptime,
                                            windowNumber: window.windowNumber, context: nil,
                                            eventNumber: 0, clickCount: 1, pressure: 1) { NSApp.sendEvent(event) }
        }
    }

    static func nativeTreeChecks() async {
        guard let table = findView(FileTreeView.FileTreeTable.self) else { return }
        window.makeFirstResponder(table)
        await measure("native_arrow_down", native: true) { key(125, characters: "\u{F701}") }
        samples[samples.count-1]["native_down_correct"] = workspace.selectedAnalysis.fileName == "file0026.js"
        await measure("native_arrow_up", native: true) { key(126, characters: "\u{F700}") }
        samples[samples.count-1]["native_up_correct"] = workspace.selectedAnalysis.fileName == "file0025.js"
        table.scrollRowToVisible(10)
        await settle()
        await measure("native_mouse_selection", native: true) {
            let rect = table.rect(ofRow: 10)
            click(table, at: NSPoint(x: rect.midX, y: rect.midY))
        }
        samples[samples.count-1]["native_click_correct"] = workspace.selectedAnalysis.fileName == "file0010.js"
        await measure("native_rapid_arrow_selection", native: true) {
            for _ in 0..<16 { key(125, characters: "\u{F701}") }
        }
        samples[samples.count-1]["native_rapid_correct"] = workspace.selectedAnalysis.fileName == "file0026.js"
        capture("17-native-selection")
    }

    static func nativeFolderChecks() async {
        guard let table = findView(FileTreeView.FileTreeTable.self),
              let row = workspace.visibleEntries.firstIndex(where: { $0.isDirectory }) else { return }
        let folder = workspace.visibleEntries[row]
        table.scrollRowToVisible(row)
        await settle()
        await measure("native_folder_selection", native: true) {
            let rect = table.rect(ofRow: row)
            click(table, at: NSPoint(x: rect.midX, y: rect.midY))
        }
        window.makeFirstResponder(table)
        await measure("native_folder_collapse", native: true) { key(123, characters: "\u{F702}") }
        samples[samples.count-1]["native_collapse_correct"] = !workspace.expandedFolders.contains(folder.id)
        await measure("native_folder_expand", native: true) { key(124, characters: "\u{F703}") }
        samples[samples.count-1]["native_expand_correct"] = workspace.expandedFolders.contains(folder.id)
        if let cell = table.view(atColumn: 0, row: table.selectedRow, makeIfNecessary: true) as? FileTreeView.Cell {
            await measure("native_disclosure_click", native: true) { click(cell.disclosure) }
            samples[samples.count-1]["native_disclosure_correct"] = !workspace.expandedFolders.contains(folder.id)
        }
    }

    static func nativeSearchCheck() async {
        guard let field = findView(NSTextField.self, matching: { $0.placeholderString == "Search files and symbols" }) else { return }
        await measure("native_type_search", native: true) {
            click(field)
            for (code, character): (UInt16, String) in [(5, "g"), (15, "r"), (14, "e"), (14, "e"), (17, "t")] {
                key(code, characters: character)
            }
        }
        samples[samples.count-1]["native_search_correct"] = workspace.searchQuery == "greet" && !workspace.searchResults.isEmpty
        let first = workspace.searchResults.first
        await measure("native_search_return", native: true) { key(36, characters: "\r") }
        samples[samples.count-1]["native_return_correct"] = first.map {
            workspace.detailTitle == "\($0.kind.capitalized): \($0.name.components(separatedBy: ".").last ?? $0.name)"
                && workspace.selectedSnippet.line == $0.line
        } ?? false
        await measure("search_function_scope") { workspace.searchKinds = "function,method,constructor" }
        await measure("native_search_signature_return", native: true) { key(36, characters: "\r") }
        samples[samples.count-1]["native_signature_correct"] = workspace.detailSignature != nil && workspace.detailTitle == "Function: greet"
        capture("18-native-search")
    }

    static func run() async {
        let repo = CommandLine.arguments[2]
        let fixtures = repo + "/Engine/Upstream/tests/fixtures/baseline"
        await settle()
        capture("01-empty")
        await measure("load_swift_fixture") { workspace.loadProject(at: fixtures + "/swift_signatures") }
        capture("02-swift-file")
        if let symbol = workspace.selectedAnalysis.symbols.first {
            await measure("inspect_swift_symbol") { workspace.inspectSymbol(symbol) }
            capture("03-swift-symbol")
        }
        // Reload immediately retains old content in the current implementation.
        workspace.loadProject(at: fixtures + "/web_app")
        capture("04-loading-stale-content")
        await measure("await_web_fixture") {}
        capture("05-web-file")
        await nativeFolderChecks()
        await measure("select_web_js") { workspace.selectedEntryID = fixtures + "/web_app/js/app.js" }
        capture("06-web-js")
        await measure("select_web_html_cached") { workspace.selectedEntryID = fixtures + "/web_app/index.html" }
        if let link = workspace.selectedAnalysis.quickLinks.first(where: { $0.type == "stylesheet" && $0.exists }) {
            await measure("open_stylesheet_link") { workspace.activateQuickLink(link) }
            samples[samples.count-1]["opened_stylesheet"] = workspace.selectedAnalysis.fileName == "style.css"
            await measure("return_to_html") { workspace.selectedEntryID = fixtures + "/web_app/index.html" }
        }
        window.setContentSize(NSSize(width: 1080, height: 560))
        await settle()
        capture("07-minimum-window")
        window.setContentSize(NSSize(width: 1240, height: 680))
        await measure("load_broken_swift") { workspace.loadProject(at: fixtures + "/swift_recovery_brace") }
        capture("08-broken-swift")
        await measure("load_port_repository") { workspace.loadProject(at: repo) }
        capture("09-port-repository")
        await measure("select_app_swift") { workspace.selectedEntryID = repo + "/Sources/LumencodeSwiftApp/LumencodeApp.swift" }
        capture("10-app-source")
        await measure("select_bridge_cpp") { workspace.selectedEntryID = repo + "/Sources/LumencodeNativeBridge/lumencode_bridge.cpp" }
        capture("11-bridge-cpp")
        await measure("select_original_parser_cpp") { workspace.selectedEntryID = repo + "/Engine/Upstream/src/symbolparser.cpp" }
        capture("16-original-parser")
        let original = (repo as NSString).deletingLastPathComponent + "/lumencode"
        if FileManager.default.fileExists(atPath: original) {
            await measure("load_original_repository") { workspace.loadProject(at: original) }
            capture("12-original-repository")
        }
        await measure("load_500_js_files") { workspace.loadProject(at: "/tmp/lumencode-audit-stress") }
        capture("13-js-stress")
        for i in 1...5 {
            await measure("stress_uncached_\(i)") { workspace.selectedEntryID = String(format: "/private/tmp/lumencode-audit-stress/file%04d.js", i) }
        }
        for i in 1...5 {
            await measure("stress_cached_\(i)") { workspace.selectedEntryID = String(format: "/private/tmp/lumencode-audit-stress/file%04d.js", i) }
        }
        let stressFile = "/private/tmp/lumencode-audit-stress/file0001.js"
        let old = try! String(contentsOfFile: stressFile, encoding: .utf8)
        try! "export function changedOnDisk() { return 2; }\n".write(toFile: stressFile, atomically: true, encoding: .utf8)
        await measure("reselect_after_disk_edit") { workspace.selectedEntryID = stressFile }
        samples[samples.count-1]["shows_changed_symbol"] = workspace.selectedAnalysis.symbols.contains { $0.name == "changedOnDisk" }
        try! old.write(toFile: stressFile, atomically: true, encoding: .utf8)
        try! FileManager.default.removeItem(atPath: stressFile)
        await measure("refresh_after_disk_delete") { workspace.refreshCurrentFile() }
        samples[samples.count-1]["cleared_deleted_symbols"] = workspace.selectedAnalysis.symbols.isEmpty
            && workspace.selectedAnalysis.summary == "Unable to read file"
        capture("19-deleted-file")
        try! old.write(toFile: stressFile, atomically: true, encoding: .utf8)
        await measure("refresh_restored_file") { workspace.refreshCurrentFile() }
        samples[samples.count-1]["restored_file_correct"] = workspace.selectedAnalysis.symbols.contains { $0.name == "helper1" }
        // Model driven rapid selection; the final state must agree with the last request.
        await measure("rapid_selection") {
            for i in 10...25 { workspace.selectedEntryID = String(format: "/private/tmp/lumencode-audit-stress/file%04d.js", i) }
        }
        await nativeTreeChecks()
        await measure("native_selection_during_project_replacement", native: true) {
            if let table = findView(FileTreeView.FileTreeTable.self) {
                window.makeFirstResponder(table)
                key(125, characters: "\u{F701}")
            }
            workspace.loadProject(at: fixtures + "/swift_signatures")
        }
        samples[samples.count-1]["native_replacement_correct"] = workspace.selectedAnalysis.fileName == "Greeter.swift"
        await nativeSearchCheck()
        await measure("search_symbols") { workspace.searchQuery = "greet" }
        samples[samples.count-1]["search_results"] = workspace.searchResults.count
        capture("14-symbol-search")
        if let result = workspace.searchResults.first(where: { $0.name == "Greeter.greet" }) {
            await measure("open_search_result") { workspace.openSearchResult(result) }
            samples[samples.count-1]["opened_expected_symbol"] = workspace.detailTitle == "Function: greet"
            capture("15-search-signature")
        }
        await measure("search_file_scope") { workspace.searchKinds = "file" }
        samples[samples.count-1]["file_scope_correct"] = !workspace.searchResults.isEmpty && workspace.searchResults.allSatisfy { $0.kind == "file" }
        if let dependency = workspace.selectedAnalysis.dependencies.first {
            await measure("inspect_dependency_after_signature") { workspace.inspectDependency(dependency) }
            samples[samples.count-1]["cleared_signature"] = workspace.detailSignature == nil
        }
        let expected = ["shows_changed_symbol", "cleared_deleted_symbols", "restored_file_correct", "opened_expected_symbol", "file_scope_correct", "opened_stylesheet", "cleared_signature",
                        "native_down_correct", "native_up_correct", "native_click_correct", "native_rapid_correct",
                        "native_search_correct", "native_return_correct", "native_signature_correct", "native_replacement_correct",
                        "native_collapse_correct", "native_expand_correct", "native_disclosure_correct"]
        let failedChecks = expected.filter { key in !samples.contains { $0[key] as? Bool == true } }
        let completed = !samples.contains { $0["timed_out"] as? Bool == true || $0["selection_matches_request"] as? Bool == false || ($0["error"] as? String ?? "") != "" }
        failed = !completed || !failedChecks.isEmpty
        let budget = ProcessInfo.processInfo.environment["LUMENCODE_AUDIT_HEARTBEAT_BUDGET_MS"].flatMap(Double.init)
        let overBudget = samples.filter { ($0["heartbeat_max_gap_ms"] as? Double ?? 0) > (budget ?? .infinity) }.compactMap { $0["name"] as? String }
        failed = failed || !overBudget.isEmpty
        let result: [String: Any] = ["method": "Actual app ContentView and WorkspaceViewModel hosted in a test NSWindow; model actions plus native mouse/key NSEvents through NSApplication.sendEvent; 10ms main-runloop heartbeat; Debug build. Screenshots excluded from timed intervals. Not OS HID delivery, production menus or frame latency.", "completed_without_timeout_or_analysis_error": completed, "functional_checks_passed": failedChecks.isEmpty, "failed_checks": failedChecks, "heartbeat_budget_ms": budget as Any? ?? NSNull(), "over_budget_samples": overBudget, "samples": samples]
        let data = try! JSONSerialization.data(withJSONObject: result, options: [.prettyPrinted, .sortedKeys])
        try! data.write(to: output.appendingPathComponent("gui.json"))
    }
}
