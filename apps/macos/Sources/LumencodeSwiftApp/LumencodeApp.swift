import AppKit
import SwiftUI
#if canImport(LumencodeCore)
import LumencodeCore
#endif

@main
struct LumencodeApp: App {
    @StateObject private var workspace = WorkspaceViewModel()

    var body: some Scene {
        WindowGroup {
            ContentView(workspace: workspace)
                .frame(minWidth: 1080, minHeight: 560)
                .onAppear {
                    guard workspace.rootPath.isEmpty else { return }
                    if let folder = CommandLine.arguments.dropFirst().first(where: {
                        var isDirectory: ObjCBool = false
                        return FileManager.default.fileExists(atPath: $0, isDirectory: &isDirectory) && isDirectory.boolValue
                    }) {
                        workspace.loadProject(at: folder)
                    }
                }
        }
        .defaultSize(width: 1240, height: 680)
        .commands {
            CommandGroup(after: .newItem) {
                Button("Open Folder…") {
                    workspace.openFolder()
                }
                .keyboardShortcut("o")
                Button("Refresh File") { workspace.refreshCurrentFile() }
                    .keyboardShortcut("r")
                Button("Find in Project") { workspace.searchFocusRequested.toggle() }
                    .keyboardShortcut("f")
            }
        }
    }
}

@MainActor
final class WorkspaceViewModel: ObservableObject {
    enum InspectionSelection: Equatable {
        case file
        case symbol(UUID)
        case member(UUID)
        case dependency(UUID)
        case route(UUID)
        case quickLink(UUID)
        case relation(UUID)
        case crossFileCall(String, String) // (fromName, toName)
    }

    @Published var rootPath = ""
    @Published var scanResult = ProjectScanResult(entries: [], projectSummary: .empty, selectedPath: "")
    @Published var selectedEntryID: String? {
        didSet {
            guard selectedEntryID != oldValue else {
                return
            }
            selectCurrentEntry()
        }
    }
    @Published var selectedAnalysis = FileAnalysis.skeleton(path: "", fileName: "", language: "")
    @Published var selectedSnippet = SnippetPayload.empty
    @Published var detailTitle = "Inspector"
    @Published var detailSubtitle = "Select a file to inspect its structure."
    @Published var detailMembers: [SymbolItem] = []
    @Published var detailCalls: [SymbolRelationItem] = []
    @Published var detailCalledBy: [SymbolRelationItem] = []
    @Published var detailCrossFileCalls: [CallEdge] = []
    @Published var detailCrossFileCalledBy: [CallEdge] = []
    @Published var statusMessage = "Open a project folder to inspect its structure."
    @Published var errorMessage = ""
    @Published var inspectionSelection: InspectionSelection = .file
    @Published var isScanning = false
    @Published var isAnalyzingFile = false
    @Published var isIndexing = false
    @Published var detailSignature: SignatureInfo?
    @Published var searchQuery = "" { didSet { if searchQuery != oldValue { updateSearch() } } }
    @Published var searchKinds = "" { didSet { if searchKinds != oldValue { updateSearch() } } }
    @Published var searchResults: [SymbolSearchResult] = []
    @Published var isSearching = false
    @Published var searchFocusRequested = false
    @Published var expandedFolders: Set<String> = []

    private var scanTask: Task<Void, Never>?
    private var analysisTask: Task<Void, Never>?
    private var analysisRequest: AnalysisRequest?
    private var indexRequest: AnalysisRequest?
    private var indexTask: Task<Void, Never>?
    private var searchTask: Task<Void, Never>?
    private var searchRequest: AnalysisRequest?
    private var generation = UUID()
    fileprivate var projectGeneration: UUID { generation }
    private var indexReady = false
    private struct FileRevision: Equatable {
        let modified: Date?
        let size: UInt64
        let inode: UInt64
        init(_ path: String) {
            let attributes = (try? FileManager.default.attributesOfItem(atPath: path)) ?? [:]
            modified = attributes[.modificationDate] as? Date
            size = (attributes[.size] as? NSNumber)?.uint64Value ?? 0
            inode = (attributes[.systemFileNumber] as? NSNumber)?.uint64Value ?? 0
        }
    }
    private struct CachedAnalysis {
        let revision: FileRevision
        let analysis: FileAnalysis
        let indexed: Bool
    }
    private var analysisCache: [String: CachedAnalysis] = [:]
    private var pendingSymbolName: String?
    private var pendingSymbolLine: Int?

    deinit {
        analysisRequest?.cancel()
        indexRequest?.cancel()
        searchRequest?.cancel()
    }

    func refreshCurrentFile() {
        if let path = selectedEntryID { analysisCache[path] = nil }
        selectCurrentEntry()
    }

    private func updateSearch() {
        searchTask?.cancel()
        searchRequest?.cancel()
        if !searchResults.isEmpty { searchResults = [] }
        let query = searchQuery.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !query.isEmpty, !rootPath.isEmpty else { isSearching = false; return }
        if !isSearching { isSearching = true }
        let root = rootPath
        let kinds = searchKinds
        let projectGeneration = generation
        let request = AnalysisRequest()
        searchRequest = request
        searchTask = Task { [weak self] in
            do { try await Task.sleep(for: .milliseconds(180)) } catch { return }
            let result = await Task.detached(priority: .userInitiated) {
                Result { try NativeAnalyzer().search(query: query, rootPath: root, kinds: kinds, request: request) }
            }.value
            guard let self, !Task.isCancelled, self.generation == projectGeneration, self.searchRequest === request else { return }
            self.isSearching = false
            switch result {
            case .success(let matches): self.searchResults = matches
            case .failure(let error): self.errorMessage = "Search unavailable: \(error.localizedDescription)"
            }
        }
    }

    func openSearchResult(_ result: SymbolSearchResult) {
        let path = WorkspaceSupport.resolvePath(result.path, relativeTo: rootPath)
        if result.kind != "file" {
            pendingSymbolName = result.name.components(separatedBy: ".").last
            pendingSymbolLine = result.line
        }
        if selectedEntryID == path {
            if result.kind == "file" { resetToFileContext() }
            else { applyPendingSymbol() }
        } else {
            selectedEntryID = path
        }
    }

    var isInspectingSymbolContext: Bool {
        switch inspectionSelection {
        case .symbol, .member, .relation:
            return true
        default:
            return false
        }
    }

    var entries: [FileTreeEntry] {
        scanResult.entries
    }

    var selectedEntry: FileTreeEntry? {
        guard let selectedEntryID else {
            return nil
        }
        return entries.first { $0.id == selectedEntryID }
    }

    // Entries are depth-first ordered, so a collapsed folder hides every following deeper entry.
    var visibleEntries: [FileTreeEntry] {
        var visible: [FileTreeEntry] = []
        var hiddenBelowDepth: Int?
        for entry in entries {
            if let depth = hiddenBelowDepth {
                if entry.depth > depth {
                    continue
                }
                hiddenBelowDepth = nil
            }
            visible.append(entry)
            if entry.isDirectory && !expandedFolders.contains(entry.id) {
                hiddenBelowDepth = entry.depth
            }
        }
        return visible
    }

    func toggleFolder(_ entry: FileTreeEntry) {
        if expandedFolders.contains(entry.id) {
            expandedFolders.remove(entry.id)
        } else {
            expandedFolders.insert(entry.id)
        }
    }

    private func revealInTree(_ path: String) {
        var revealed = expandedFolders
        var parent = (path as NSString).deletingLastPathComponent
        while parent.count > rootPath.count, parent.hasPrefix(rootPath) {
            revealed.insert(parent)
            parent = (parent as NSString).deletingLastPathComponent
        }
        if revealed != expandedFolders { expandedFolders = revealed }
    }

    func openFolder() {
        let panel = NSOpenPanel()
        panel.title = "Open Project Folder"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false

        if panel.runModal() == .OK, let url = panel.url {
            loadProject(at: url.path)
        }
    }

    func loadProject(at path: String) {
        let absolutePath = WorkspaceSupport.resolvePath(path, relativeTo: FileManager.default.currentDirectoryPath)
        scanTask?.cancel()
        analysisTask?.cancel()
        analysisRequest?.cancel()
        indexTask?.cancel()
        indexRequest?.cancel()
        searchTask?.cancel()
        searchRequest?.cancel()
        generation = UUID()
        let projectGeneration = generation
        indexReady = false
        isIndexing = false
        isAnalyzingFile = false
        searchQuery = ""
        searchResults = []
        analysisCache = [:]
        expandedFolders = []
        selectedEntryID = nil
        scanResult = ProjectScanResult(entries: [], projectSummary: .empty, selectedPath: "")
        rootPath = absolutePath
        selectedAnalysis = .skeleton(path: "", fileName: "", language: "")
        resetToFileContext()
        selectedSnippet = .empty
        pendingSymbolName = nil
        pendingSymbolLine = nil
        errorMessage = ""
        statusMessage = "Scanning \((absolutePath as NSString).lastPathComponent)…"
        isScanning = true

        scanTask = Task { [weak self] in
            let result = await Task.detached(priority: .userInitiated) {
                ProjectScanner().scan(rootPath: absolutePath, selectedPath: nil)
            }.value
            guard let self, !Task.isCancelled, self.generation == projectGeneration else {
                return
            }
            self.isScanning = false
            self.scanResult = result
            self.statusMessage = "\(result.projectSummary.totalFiles) files, \(result.projectSummary.totalDirectories) folders"

            if let selectedPath = result.selectedPath.isEmpty ? result.entries.first(where: { !$0.isDirectory })?.id : result.selectedPath {
                self.selectedEntryID = selectedPath
                self.startProjectIndex()
            } else {
                self.statusMessage = "No supported files were found in this folder."
                self.selectedAnalysis = FileAnalysis.skeleton(path: "", fileName: "", language: "")
                self.resetToFileContext()
                self.detailTitle = "Inspector"
                self.detailSubtitle = "Open a folder with supported source files."
            }
        }
    }

    func inspectSymbol(_ symbol: SymbolItem) {
        inspectionSelection = .symbol(symbol.id)
        detailTitle = "\(symbol.kind.capitalized): \(symbol.name)"
        detailSubtitle = symbol.detail.isEmpty ? "Line \(symbol.line)" : symbol.detail
        detailMembers = symbol.members
        detailCalls = symbol.calls
        detailCalledBy = symbol.calledBy
        detailSignature = symbol.signature

        // Get cross-file call edges for this symbol
        detailCrossFileCalls = selectedAnalysis.callEdges.filter { $0.fromName == symbol.name }
        detailCrossFileCalledBy = selectedAnalysis.callEdges.filter { $0.toName == symbol.name }

        selectedSnippet = WorkspaceSupport.snippetPayload(
            from: symbol,
            fallbackPath: selectedAnalysis.path,
            fallbackLanguage: selectedAnalysis.language
        )
    }

    func inspectMember(_ symbol: SymbolItem) {
        inspectionSelection = .member(symbol.id)
        detailTitle = "\(symbol.kind.capitalized): \(symbol.name)"
        detailSubtitle = symbol.detail.isEmpty ? "Line \(symbol.line)" : symbol.detail
        detailMembers = symbol.members
        detailCalls = symbol.calls
        detailCalledBy = symbol.calledBy
        detailSignature = symbol.signature

        // Get cross-file call edges for this symbol
        detailCrossFileCalls = selectedAnalysis.callEdges.filter { $0.fromName == symbol.name }
        detailCrossFileCalledBy = selectedAnalysis.callEdges.filter { $0.toName == symbol.name }

        selectedSnippet = WorkspaceSupport.snippetPayload(
            from: symbol,
            fallbackPath: selectedAnalysis.path,
            fallbackLanguage: selectedAnalysis.language
        )
    }

    func inspectDependency(_ dependency: DependencyItem) {
        detailSignature = nil
        inspectionSelection = .dependency(dependency.id)
        detailTitle = "Dependency: \(dependency.label)"
        detailSubtitle = dependency.detail.isEmpty ? dependency.target : dependency.detail
        detailMembers = []
        detailCalls = []
        detailCalledBy = []
        selectedSnippet = WorkspaceSupport.snippetPayload(
            from: dependency,
            fallbackPath: selectedAnalysis.path,
            fallbackLanguage: selectedAnalysis.language
        )
    }

    func inspectRoute(_ route: RouteItem) {
        detailSignature = nil
        inspectionSelection = .route(route.id)
        detailTitle = route.label.isEmpty ? "\(route.method) \(route.path)" : route.label
        detailSubtitle = route.detail.isEmpty ? "Route" : route.detail
        detailMembers = []
        detailCalls = []
        detailCalledBy = []
        selectedSnippet = WorkspaceSupport.snippetPayload(
            from: route,
            fallbackPath: selectedAnalysis.path,
            fallbackLanguage: selectedAnalysis.language
        )
    }

    func inspectQuickLink(_ quickLink: QuickLinkItem) {
        detailSignature = nil
        inspectionSelection = .quickLink(quickLink.id)
        detailTitle = "Quick Link: \(quickLink.label)"
        detailSubtitle = quickLink.detail.isEmpty ? quickLink.target : quickLink.detail
        detailMembers = []
        detailCalls = []
        detailCalledBy = []
        selectedSnippet = WorkspaceSupport.snippetPayload(
            from: quickLink,
            fallbackPath: selectedAnalysis.path,
            fallbackLanguage: selectedAnalysis.language
        )
    }

    func inspectRelation(_ relation: SymbolRelationItem) {
        if let path = relation.path,
           entries.contains(where: { $0.path == path && !$0.isDirectory }) {
            pendingSymbolName = relation.name
            pendingSymbolLine = relation.line
            if path == selectedAnalysis.path { applyPendingSymbol() }
            else { selectedEntryID = path }
            return
        }
        inspectionSelection = .relation(relation.id)
        detailSignature = nil
        detailTitle = "\(relation.kind.capitalized): \(relation.name)"
        detailSubtitle = relation.detail.isEmpty ? "Line \(relation.line)" : relation.detail
        detailMembers = []
        detailCalls = []
        detailCalledBy = []
        selectedSnippet = WorkspaceSupport.snippetPayload(
            from: relation,
            fallbackPath: selectedAnalysis.path,
            fallbackLanguage: selectedAnalysis.language
        )
    }

    func activateQuickLink(_ quickLink: QuickLinkItem) {
        if quickLink.exists, quickLink.target.hasPrefix("/") {
            selectedEntryID = quickLink.target
            return
        }
        inspectQuickLink(quickLink)
    }

    func openRelatedFile(_ related: RelatedFileItem) {
        guard related.exists else {
            return
        }
        selectedEntryID = related.path
    }

    func navigateToCallEdge(_ edge: CallEdge) {
        pendingSymbolName = edge.toName
        if selectedEntryID == edge.to {
            applyPendingSymbol()
        } else {
            selectedEntryID = edge.to
        }
    }

    private func applyPendingSymbol() {
        guard let name = pendingSymbolName else {
            return
        }
        pendingSymbolName = nil
        let line = pendingSymbolLine
        pendingSymbolLine = nil
        func find(_ symbols: [SymbolItem]) -> SymbolItem? {
            for symbol in symbols {
                if symbol.name == name && (line == nil || symbol.line == line) { return symbol }
                if let match = find(symbol.members) { return match }
            }
            return nil
        }
        if let symbol = find(selectedAnalysis.symbols) {
            inspectSymbol(symbol)
        }
    }

    func resetToFileContext() {
        inspectionSelection = .file
        detailTitle = selectedAnalysis.fileName.isEmpty ? "Inspector" : selectedAnalysis.fileName
        detailSubtitle = selectedAnalysis.summary.isEmpty ? "Select a symbol to inspect it." : selectedAnalysis.summary
        detailMembers = []
        detailCalls = []
        detailCalledBy = []
        detailCrossFileCalls = []
        detailCrossFileCalledBy = []
        detailSignature = nil
        selectedSnippet = WorkspaceSupport.snippetPayload(from: selectedAnalysis)
    }

    var selectedItemPath: String {
        if !selectedSnippet.path.isEmpty {
            return selectedSnippet.path
        }
        return selectedAnalysis.path
    }

    var selectedItemExists: Bool {
        let path = selectedItemPath
        return !path.isEmpty && FileManager.default.fileExists(atPath: path)
    }

    func openSelectedItemInDefaultEditor() {
        let path = selectedItemPath
        guard !path.isEmpty else {
            return
        }
        NSWorkspace.shared.open(URL(fileURLWithPath: path))
    }

    func revealSelectedItemInFinder() {
        let path = selectedItemPath
        guard !path.isEmpty else {
            return
        }
        NSWorkspace.shared.selectFile(path, inFileViewerRootedAtPath: NSString(string: path).deletingLastPathComponent)
    }

    private func selectCurrentEntry() {
        analysisTask?.cancel()
        analysisRequest?.cancel()
        analysisRequest = nil
        isAnalyzingFile = false
        guard let entry = selectedEntry else {
            return
        }

        errorMessage = ""
        // SwiftUI's table delegate may still be applying selection. Reveal folders
        // on the next turn so the table's row set does not mutate reentrantly.
        let selectedPath = entry.path
        DispatchQueue.main.async { [weak self] in
            guard let self, self.selectedEntryID == selectedPath else { return }
            self.revealInTree(selectedPath)
        }

        if entry.isDirectory {
            detailSignature = nil
            let childCount = entries.filter {
                $0.path.hasPrefix(entry.path + "/") && $0.depth == entry.depth + 1
            }.count
            selectedAnalysis = FileAnalysis(
                path: entry.path,
                fileName: entry.name,
                language: "folder",
                symbols: [],
                quickLinks: [],
                dependencies: [],
                routes: [],
                relatedFiles: [],
                packageSummary: PackageSummary(),
                cssSummary: CSSSummary(),
                summary: "\(childCount) direct child item\(childCount == 1 ? "" : "s")"
            )
            detailTitle = entry.name
            detailSubtitle = selectedAnalysis.summary
            detailMembers = []
            detailCalls = []
            detailCalledBy = []
            selectedSnippet = WorkspaceSupport.snippetPayload(
                kind: "folder",
                name: entry.name,
                path: entry.path,
                language: "",
                line: 0,
                snippet: selectedAnalysis.summary,
                detail: "",
                snippetKind: "context_excerpt",
                diagnosticsMode: "none"
            )
            inspectionSelection = .file
            statusMessage = "Browsing folder \(entry.name)"
            return
        }

        if let cached = analysisCache[entry.path], cached.revision == FileRevision(entry.path), cached.indexed == indexReady {
            showAnalysis(cached.analysis, for: entry)
            return
        }

        statusMessage = "Analyzing \(entry.name)…"
        isAnalyzingFile = true
        let path = entry.path
        let root = rootPath
        let projectGeneration = generation
        let revision = FileRevision(path)
        let indexed = indexReady
        let request = AnalysisRequest()
        analysisRequest = request
        selectedAnalysis = .skeleton(path: path, fileName: entry.name, language: entry.fileType)
        resetToFileContext()
        analysisTask = Task { [weak self] in
            let result = await Task.detached(priority: .userInitiated) { () -> Result<FileAnalysis, any Error> in
                return Result {
                    try NativeAnalyzer().analyzeFile(path: path, rootPath: indexed ? root : nil, request: request)
                }
            }.value
            guard let self, !Task.isCancelled, self.generation == projectGeneration,
                  self.analysisRequest === request, self.selectedEntryID == path else {
                return
            }
            self.isAnalyzingFile = false
            switch result {
            case .success(let analysis):
                guard revision == FileRevision(path) else { self.selectCurrentEntry(); return }
                self.analysisCache[path] = CachedAnalysis(revision: revision, analysis: analysis, indexed: indexed)
                self.showAnalysis(analysis, for: entry)
            case .failure(let error):
                self.selectedAnalysis = FileAnalysis.skeleton(path: path, fileName: entry.name, language: entry.fileType)
                self.selectedAnalysis.summary = "Analysis failed: \(error)"
                self.resetToFileContext()
                self.errorMessage = "Failed to analyze \(entry.name)."
            }
        }
    }

    private func startProjectIndex() {
        let root = rootPath
        let projectGeneration = generation
        let request = AnalysisRequest()
        indexRequest = request
        isIndexing = true
        indexTask = Task { [weak self] in
            let result = await Task.detached(priority: .utility) {
                Result { try NativeAnalyzer().buildIndex(rootPath: root, request: request) }
            }.value
            guard let self, !Task.isCancelled, self.generation == projectGeneration else { return }
            self.isIndexing = false
            switch result {
            case .success:
                self.indexReady = true
                // Preserve an active symbol inspection; its next file visit uses indexed data.
                if self.inspectionSelection == .file { self.selectCurrentEntry() }
            case .failure(let error):
                self.errorMessage = "Project relationships unavailable: \(error.localizedDescription)"
            }
        }
    }

    private func showAnalysis(_ analysis: FileAnalysis, for entry: FileTreeEntry) {
        selectedAnalysis = analysis
        resetToFileContext()
        statusMessage = "Analyzed \(entry.name)"
        applyPendingSymbol()
    }
}

struct ContentView: View {
    @ObservedObject var workspace: WorkspaceViewModel
    @FocusState private var searchFocused: Bool

    var body: some View {
        VSplitView {
            HSplitView {
                sidebar
                    .frame(minWidth: 280, idealWidth: 320, maxWidth: 420)
                overviewPane
                    .id(workspace.selectedEntryID)
                    .frame(minWidth: 340, idealWidth: 420)
                detailPane
                    .frame(minWidth: 320, idealWidth: 400)
            }
            codePane
                .frame(minHeight: 220, idealHeight: 280)
        }
        .background(Color(nsColor: .windowBackgroundColor))
        .preferredColorScheme(.dark)
        .onChange(of: workspace.searchFocusRequested) { _, _ in searchFocused = true }
        .toolbar {
            ToolbarItemGroup {
                Button("Open Folder…") {
                    workspace.openFolder()
                }
                Text(workspace.rootPath.isEmpty ? "No folder open" : workspace.rootPath)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
        }
        .overlay(alignment: .bottomLeading) {
            if !workspace.errorMessage.isEmpty {
                Text(workspace.errorMessage)
                    .font(.caption)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                    .background(.regularMaterial)
                    .clipShape(RoundedRectangle(cornerRadius: 10))
                    .padding()
            }
        }
    }

    private var sidebar: some View {
        VStack(alignment: .leading, spacing: 12) {
            headerCard(title: "Project", subtitle: workspace.statusMessage)
                .overlay(alignment: .topTrailing) {
                    if workspace.isScanning || workspace.isAnalyzingFile || workspace.isIndexing {
                        ProgressView()
                            .controlSize(.small)
                            .padding(16)
                    }
                }

            if !workspace.rootPath.isEmpty {
                TextField("Search files and symbols", text: $workspace.searchQuery)
                    .textFieldStyle(.roundedBorder)
                    .focused($searchFocused)
                    .onSubmit {
                        if let first = workspace.searchResults.first { workspace.openSearchResult(first) }
                    }
                if !workspace.searchQuery.isEmpty {
                    Picker("Search scope", selection: $workspace.searchKinds) {
                        Text("All").tag("")
                        Text("Files").tag("file")
                        Text("Functions").tag("function,method,constructor")
                        Text("Types").tag("class,struct,interface,enum,type,trait,object,module")
                    }
                    .labelsHidden()
                    .pickerStyle(.menu)
                }
            }

            if workspace.isScanning {
                Spacer()
                ProgressView("Scanning folder…")
                    .frame(maxWidth: .infinity)
                Spacer()
            } else if workspace.entries.isEmpty {
                Spacer()
                ContentUnavailableView(
                    "No Project Open",
                    systemImage: "folder",
                    description: Text("Choose a folder to start exploring.")
                )
                Spacer()
            } else if !workspace.searchQuery.isEmpty {
                searchResultsPane
            } else {
                if workspace.isIndexing {
                    Text("Building project relationships…")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                FileTreeView(workspace: workspace)

            }
        }
        .padding(16)
        .background(shellPanel)
    }

    private var searchResultsPane: some View {
        VStack(alignment: .leading) {
            if workspace.isSearching { ProgressView("Searching…").controlSize(.small) }
            else if workspace.searchResults.isEmpty { Text("No matches").foregroundStyle(.secondary) }
            List(workspace.searchResults) { result in
                Button { workspace.openSearchResult(result) } label: {
                    VStack(alignment: .leading, spacing: 3) {
                        Text(result.name).font(.body.weight(.medium))
                        Text("\(result.kind) · \(result.path):\(result.line)")
                            .font(.caption).foregroundStyle(.secondary).lineLimit(2)
                    }
                }
                .buttonStyle(.plain)
            }
            .listStyle(.sidebar)
            .scrollContentBackground(.hidden)
            .background(Color.clear)
        }
    }

    private var overviewPane: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                projectSummaryCard
                overviewSection
            }
            .padding(16)
        }
        .background(contentPanel)
    }

    private var detailPane: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                headerCard(title: workspace.detailTitle, subtitle: workspace.detailSubtitle)

                if let signature = workspace.detailSignature,
                   !signature.parameters.isEmpty || signature.returnType != nil {
                    sectionCard(title: "Signature") {
                        ForEach(Array(signature.parameters.enumerated()), id: \.offset) { _, parameter in
                            Text(parameter.name + (parameter.type.map { ": " + $0 } ?? "") + (parameter.defaultValue.map { " = " + $0 } ?? ""))
                                .font(.system(.caption, design: .monospaced))
                                .textSelection(.enabled)
                        }
                        if let returns = signature.returnType {
                            Text("Returns: " + returns)
                                .font(.system(.caption, design: .monospaced))
                                .textSelection(.enabled)
                        }
                    }
                }

                if !workspace.detailMembers.isEmpty {
                    sectionCard(title: "Members") {
                        ForEach(workspace.detailMembers) { member in
                            selectionRow(
                                title: member.name,
                                subtitle: symbolSubtitle(for: member),
                                actionTitle: "Inspect"
                            ) {
                                workspace.inspectMember(member)
                            }
                        }
                    }
                }

                if workspace.isInspectingSymbolContext {
                    sectionCard(title: "Calls") {
                        if workspace.detailCalls.isEmpty && workspace.detailCrossFileCalls.isEmpty {
                            Text("No outgoing calls found for this symbol.")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        } else {
                            ForEach(workspace.detailCalls) { relation in
                                selectionRow(
                                    title: relation.name,
                                    subtitle: relation.detail.isEmpty ? "\(relation.kind) • line \(relation.line)" : relation.detail,
                                    actionTitle: "Inspect"
                                ) {
                                    workspace.inspectRelation(relation)
                                }
                            }

                            ForEach(workspace.detailCrossFileCalls) { edge in
                                HStack(alignment: .top, spacing: 12) {
                                    VStack(alignment: .leading, spacing: 4) {
                                        Text(edge.toName)
                                            .font(.headline)
                                        HStack(spacing: 8) {
                                            Text("in \(WorkspaceSupport.relativePath(edge.to, from: workspace.rootPath))")
                                                .font(.caption)
                                                .foregroundStyle(.secondary)
                                            confidenceBadge(edge.confidence)
                                        }
                                    }
                                    Spacer()
                                    Button("Navigate", action: {
                                        workspace.navigateToCallEdge(edge)
                                    })
                                    .buttonStyle(.borderedProminent)
                                    .controlSize(.small)
                                }
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .padding(.vertical, 2)
                            }
                        }
                    }
                }

                if workspace.isInspectingSymbolContext {
                    sectionCard(title: "Called By") {
                        if workspace.detailCalledBy.isEmpty && workspace.detailCrossFileCalledBy.isEmpty {
                            Text("No incoming callers found for this symbol.")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        } else {
                            ForEach(workspace.detailCalledBy) { relation in
                                selectionRow(
                                    title: relation.name,
                                    subtitle: relation.detail.isEmpty ? "\(relation.kind) • line \(relation.line)" : relation.detail,
                                    actionTitle: "Inspect"
                                ) {
                                    workspace.inspectRelation(relation)
                                }
                            }

                            ForEach(workspace.detailCrossFileCalledBy) { edge in
                                HStack(alignment: .top, spacing: 12) {
                                    VStack(alignment: .leading, spacing: 4) {
                                        Text(edge.fromName)
                                            .font(.headline)
                                        HStack(spacing: 8) {
                                            Text("in \(WorkspaceSupport.relativePath(edge.from, from: workspace.rootPath))")
                                                .font(.caption)
                                                .foregroundStyle(.secondary)
                                            confidenceBadge(edge.confidence)
                                        }
                                    }
                                    Spacer()
                                    Button("Navigate", action: {
                                        workspace.navigateToCallEdge(
                                            CallEdge(
                                                from: edge.to,
                                                to: edge.from,
                                                fromName: edge.toName,
                                                toName: edge.fromName,
                                                confidence: edge.confidence,
                                                evidence: edge.evidence,
                                                line: edge.line
                                            )
                                        )
                                    })
                                    .buttonStyle(.borderedProminent)
                                    .controlSize(.small)
                                }
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .padding(.vertical, 2)
                            }
                        }
                    }
                }

                if !workspace.selectedAnalysis.dependencies.isEmpty {
                    sectionCard(title: "Dependencies") {
                        ForEach(workspace.selectedAnalysis.dependencies) { dependency in
                            selectionRow(
                                title: dependency.label,
                                subtitle: dependency.detail.isEmpty ? "\(dependency.type) • line \(dependency.line)" : dependency.detail,
                                actionTitle: "Inspect"
                            ) {
                                workspace.inspectDependency(dependency)
                            }
                        }
                    }
                }

                if !workspace.selectedAnalysis.routes.isEmpty {
                    sectionCard(title: "Routes") {
                        ForEach(workspace.selectedAnalysis.routes) { route in
                            selectionRow(
                                title: route.label.isEmpty ? "\(route.method) \(route.path)" : route.label,
                                subtitle: route.detail.isEmpty ? "line \(route.line)" : route.detail,
                                actionTitle: "Inspect"
                            ) {
                                workspace.inspectRoute(route)
                            }
                        }
                    }
                }

                if !workspace.selectedAnalysis.relatedFiles.isEmpty {
                    sectionCard(title: "Related Files") {
                        ForEach(workspace.selectedAnalysis.relatedFiles) { related in
                            selectionRow(
                                title: related.label,
                                subtitle: related.type,
                                actionTitle: related.exists ? "Open" : "Missing"
                            ) {
                                workspace.openRelatedFile(related)
                            }
                            .disabled(!related.exists)
                        }
                    }
                }

                if !workspace.selectedAnalysis.quickLinks.isEmpty {
                    sectionCard(title: "Quick Links") {
                        ForEach(workspace.selectedAnalysis.quickLinks) { link in
                            selectionRow(
                                title: link.label,
                                subtitle: link.detail.isEmpty ? link.target : link.detail,
                                actionTitle: link.exists && link.target.hasPrefix("/") ? "Open" : "Inspect"
                            ) {
                                workspace.activateQuickLink(link)
                            }
                        }
                    }
                }

                if !workspace.selectedAnalysis.packageSummary.name.isEmpty || !workspace.selectedAnalysis.packageSummary.dependencies.isEmpty {
                    sectionCard(title: "Package") {
                        if !workspace.selectedAnalysis.packageSummary.name.isEmpty {
                            Text(workspace.selectedAnalysis.packageSummary.name + (workspace.selectedAnalysis.packageSummary.version.isEmpty ? "" : "@\(workspace.selectedAnalysis.packageSummary.version)"))
                                .font(.headline)
                        }
                        if !workspace.selectedAnalysis.packageSummary.main.isEmpty {
                            Text("Main: \(workspace.selectedAnalysis.packageSummary.main)")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        if !workspace.selectedAnalysis.packageSummary.dependencies.isEmpty {
                            Text("Dependencies: \(workspace.selectedAnalysis.packageSummary.dependencies.count)")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                }
            }
            .padding(16)
        }
        .background(contentPanel)
    }

    private var codePane: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .firstTextBaseline) {
                VStack(alignment: .leading, spacing: 4) {
                    Text(workspace.selectedSnippet.path.isEmpty ? "Source" : WorkspaceSupport.relativePath(workspace.selectedSnippet.path, from: workspace.rootPath))
                        .font(.headline)
                    if !workspace.selectedSnippet.name.isEmpty {
                        Text("\(workspace.selectedSnippet.kind): \(workspace.selectedSnippet.name)")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                Spacer()
                Button {
                    workspace.openSelectedItemInDefaultEditor()
                } label: {
                    Image(systemName: "square.and.arrow.up")
                }
                .buttonStyle(.plain)
                .help("Open in Default Editor")
                .disabled(!workspace.selectedItemExists)

                Button {
                    workspace.revealSelectedItemInFinder()
                } label: {
                    Image(systemName: "folder")
                }
                .buttonStyle(.plain)
                .help("Reveal in Finder")
                .disabled(!workspace.selectedItemExists)

                if workspace.selectedSnippet.line > 0 {
                    badge("L\(workspace.selectedSnippet.line)")
                }
                if !workspace.selectedSnippet.language.isEmpty {
                    badge(workspace.selectedSnippet.language)
                }
                if workspace.selectedSnippet.isTruncated {
                    badge("truncated")
                }
            }

            if !workspace.selectedSnippet.detail.isEmpty {
                Text(workspace.selectedSnippet.detail)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if workspace.selectedAnalysis.analysisHasAstErrors || workspace.selectedAnalysis.analysisPartial {
                Label("Syntax damage detected · \(workspace.selectedAnalysis.analysisSourceMode) · \(workspace.selectedAnalysis.analysisConfidenceLabel) confidence", systemImage: "exclamationmark.triangle.fill")
                    .font(.caption)
                    .foregroundStyle(.orange)
            }
            ForEach(workspace.selectedAnalysis.analysisNotices) { notice in
                Text(notice.message).font(.caption).foregroundStyle(.orange)
            }

            CodeSnippetView(
                text: workspace.selectedSnippet.displaySnippet.isEmpty
                    ? "Select a file, symbol, dependency, or route to inspect its source snippet."
                    : workspace.selectedSnippet.displaySnippet,
                language: workspace.selectedSnippet.language
            )
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            .background(
                RoundedRectangle(cornerRadius: 16, style: .continuous)
                    .fill(Color(red: 0.10, green: 0.11, blue: 0.14))
                    .overlay(
                        RoundedRectangle(cornerRadius: 16, style: .continuous)
                            .stroke(Color.white.opacity(0.10), lineWidth: 1)
                    )
            )
            .clipShape(RoundedRectangle(cornerRadius: 14))
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .padding(16)
        .background(shellPanel)
    }

    private var projectSummaryCard: some View {
        sectionCard(title: "Overview") {
            LabeledContent("Files", value: "\(workspace.scanResult.projectSummary.totalFiles)")
            LabeledContent("Folders", value: "\(workspace.scanResult.projectSummary.totalDirectories)")
            if !workspace.scanResult.projectSummary.mainEntryPoint.isEmpty {
                LabeledContent("Entry", value: WorkspaceSupport.relativePath(workspace.scanResult.projectSummary.mainEntryPoint, from: workspace.rootPath))
            }
            if !workspace.selectedAnalysis.path.isEmpty {
                LabeledContent("Selection", value: workspace.selectedAnalysis.fileName)
            }
        }
    }

    private struct SymbolRow: Identifiable {
        let id: String
        let symbol: SymbolItem
        let isMember: Bool
    }

    private var overviewRows: [SymbolRow] {
        workspace.selectedAnalysis.symbols.flatMap { symbol in
            [SymbolRow(id: symbol.id.uuidString, symbol: symbol, isMember: false)] +
            symbol.members.map { member in
                SymbolRow(id: symbol.id.uuidString + member.id.uuidString, symbol: member, isMember: true)
            }
        }
    }

    @ViewBuilder
    private var overviewSection: some View {
        if let entry = workspace.selectedEntry, !entry.isDirectory {
            if !workspace.selectedAnalysis.symbols.isEmpty {
                LazyVStack(alignment: .leading, spacing: 12) {
                    Text("Symbols").font(.headline)
                    ForEach(overviewRows) { row in
                        HStack(spacing: 12) {
                            if row.isMember {
                                Rectangle().fill(Color.secondary.opacity(0.3)).frame(width: 2)
                            }
                            selectionRow(
                                title: row.symbol.name,
                                subtitle: symbolSubtitle(for: row.symbol),
                                actionTitle: "Inspect"
                            ) {
                                if row.isMember { workspace.inspectMember(row.symbol) }
                                else { workspace.inspectSymbol(row.symbol) }
                            }
                        }
                        .padding(.leading, row.isMember ? 16 : 0)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(16)
                .background(cardPanel)
            } else {
                headerCard(title: "No Symbols", subtitle: "No structured declarations found in this file.")
            }
        } else {
            headerCard(title: "Folder Summary", subtitle: workspace.selectedAnalysis.summary)
        }
    }

    private func headerCard(title: String, subtitle: String) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(.title3.weight(.semibold))
            Text(subtitle)
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
        .background(cardPanel)
    }

    private func sectionCard<Content: View>(title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(title)
                .font(.headline)
            content()
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
        .background(cardPanel)
    }

    private func selectionRow(title: String, subtitle: String, actionTitle: String, action: @escaping () -> Void) -> some View {
        HStack(alignment: .top, spacing: 12) {
            VStack(alignment: .leading, spacing: 4) {
                Text(title)
                    .font(.headline)
                Text(subtitle)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Button(actionTitle, action: action)
                .buttonStyle(.borderedProminent)
                .controlSize(.small)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.vertical, 2)
    }

    private func symbolSubtitle(for symbol: SymbolItem) -> String {
        var parts = [symbol.kind, "line \(symbol.line)"]
        if !symbol.detail.isEmpty {
            parts.append(symbol.detail)
        }
        if !symbol.calls.isEmpty {
            parts.append("\(symbol.calls.count) call\(symbol.calls.count == 1 ? "" : "s")")
        }
        if !symbol.calledBy.isEmpty {
            parts.append("\(symbol.calledBy.count) caller\(symbol.calledBy.count == 1 ? "" : "s")")
        }
        return parts.joined(separator: " • ")
    }

    private func badge(_ text: String) -> some View {
        Text(text)
            .font(.caption2.weight(.semibold))
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(Color.white.opacity(0.10), in: Capsule())
    }

    private func confidenceBadge(_ confidence: String) -> some View {
        let (color, label) = confidence == "high" ? (Color.green, "high") :
                             confidence == "medium" ? (Color.yellow, "medium") :
                             (Color.gray, "low")
        return Text(label)
            .font(.caption2.weight(.semibold))
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(color.opacity(0.2), in: Capsule())
            .foregroundColor(color)
    }

    private var shellPanel: some View {
        RoundedRectangle(cornerRadius: 24, style: .continuous)
            .fill(Color(red: 0.13, green: 0.14, blue: 0.17))
            .overlay(
                RoundedRectangle(cornerRadius: 24, style: .continuous)
                    .stroke(Color.white.opacity(0.08), lineWidth: 1)
            )
            .shadow(color: Color.black.opacity(0.25), radius: 16, y: 8)
    }

    private var cardPanel: some View {
        RoundedRectangle(cornerRadius: 18, style: .continuous)
            .fill(Color(red: 0.17, green: 0.18, blue: 0.22))
            .overlay(
                RoundedRectangle(cornerRadius: 18, style: .continuous)
                    .stroke(Color.white.opacity(0.08), lineWidth: 1)
            )
    }

    private var contentPanel: some View {
        RoundedRectangle(cornerRadius: 24, style: .continuous)
            .fill(Color(red: 0.15, green: 0.16, blue: 0.19))
            .overlay(
                RoundedRectangle(cornerRadius: 24, style: .continuous)
                    .stroke(Color.white.opacity(0.08), lineWidth: 1)
            )
            .shadow(color: Color.black.opacity(0.18), radius: 12, y: 6)
    }


}

// AppKit owns row selection and keyboard navigation. Updating its snapshot never
// publishes back into SwiftUI from inside a table delegate/update callback.
private struct FileTreeView: NSViewRepresentable {
    @ObservedObject var workspace: WorkspaceViewModel

    func makeCoordinator() -> Coordinator { Coordinator(workspace: workspace) }

    func makeNSView(context: Context) -> NSScrollView {
        let scroll = NSScrollView()
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        let table = FileTreeTable()
        table.setAccessibilityIdentifier("project-file-tree")
        table.setAccessibilityLabel("Project files")
        table.headerView = nil
        table.style = .sourceList
        table.backgroundColor = .clear
        scroll.contentView.drawsBackground = false
        table.rowHeight = 26
        table.intercellSpacing = NSSize(width: 0, height: 2)
        table.allowsEmptySelection = true
        table.columnAutoresizingStyle = .lastColumnOnlyAutoresizingStyle
        let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("file"))
        column.resizingMask = .autoresizingMask
        table.addTableColumn(column)
        table.delegate = context.coordinator
        table.dataSource = context.coordinator
        table.folderKey = { [weak coordinator = context.coordinator] expand in coordinator?.folderKey(expand: expand) }
        scroll.documentView = table
        return scroll
    }

    func updateNSView(_ view: NSScrollView, context: Context) {
        guard let table = view.documentView as? FileTreeTable else { return }
        context.coordinator.update(table: table)
    }

    final class FileTreeTable: NSTableView {
        var folderKey: ((Bool) -> Void)?
        override func keyDown(with event: NSEvent) {
            if event.keyCode == 124 { folderKey?(true) }
            else if event.keyCode == 123 { folderKey?(false) }
            else { super.keyDown(with: event) }
        }
    }

    final class Cell: NSTableCellView {
        let disclosure = NSButton()
        let icon = NSImageView()
        let label = NSTextField(labelWithString: "")
        var leading: NSLayoutConstraint!
        var toggle: (() -> Void)?

        override init(frame: NSRect) {
            super.init(frame: frame)
            disclosure.isBordered = false
            disclosure.title = ""
            disclosure.imagePosition = .imageOnly
            disclosure.setButtonType(.onOff)
            disclosure.target = self
            disclosure.action = #selector(disclose)
            disclosure.refusesFirstResponder = true
            label.lineBreakMode = .byTruncatingMiddle
            label.font = .systemFont(ofSize: NSFont.systemFontSize)
            for child in [disclosure, icon, label] {
                child.translatesAutoresizingMaskIntoConstraints = false
                addSubview(child)
                child.centerYAnchor.constraint(equalTo: centerYAnchor).isActive = true
            }
            leading = disclosure.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 4)
            NSLayoutConstraint.activate([
                leading,
                disclosure.widthAnchor.constraint(equalToConstant: 14),
                disclosure.heightAnchor.constraint(equalToConstant: 18),
                icon.leadingAnchor.constraint(equalTo: disclosure.trailingAnchor, constant: 4),
                icon.widthAnchor.constraint(equalToConstant: 16),
                icon.heightAnchor.constraint(equalToConstant: 16),
                label.leadingAnchor.constraint(equalTo: icon.trailingAnchor, constant: 6),
                label.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -6)
            ])
            textField = label
        }
        required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
        @objc private func disclose() { toggle?() }
    }

    @MainActor
    final class Coordinator: NSObject, NSTableViewDataSource, NSTableViewDelegate {
        let workspace: WorkspaceViewModel
        var entries: [FileTreeEntry] = []
        var expanded: Set<String> = []
        var root = ""
        var updating = false
        weak var table: NSTableView?

        init(workspace: WorkspaceViewModel) { self.workspace = workspace }

        func update(table: NSTableView) {
            self.table = table
            let next = workspace.visibleEntries
            let changedRoot = root != workspace.rootPath
            updating = true
            defer { updating = false }
            if changedRoot || entries != next || expanded != workspace.expandedFolders {
                root = workspace.rootPath
                entries = next
                expanded = workspace.expandedFolders
                table.reloadData()
                if changedRoot { table.enclosingScrollView?.contentView.scroll(to: .zero) }
            }
            let row = entries.firstIndex { $0.id == workspace.selectedEntryID }
            if table.selectedRow != (row ?? -1) {
                if let row {
                    table.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
                    table.scrollRowToVisible(row)
                } else { table.deselectAll(nil) }
            }
        }

        func numberOfRows(in tableView: NSTableView) -> Int { entries.count }

        func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
            guard entries.indices.contains(row) else { return nil }
            let entry = entries[row]
            let identifier = NSUserInterfaceItemIdentifier("file-cell")
            let cell = tableView.makeView(withIdentifier: identifier, owner: self) as? Cell ?? Cell()
            cell.identifier = identifier
            cell.leading.constant = 4 + CGFloat(entry.depth) * 14
            cell.label.stringValue = entry.name
            cell.toolTip = entry.path
            cell.disclosure.isHidden = !entry.isDirectory
            cell.disclosure.state = expanded.contains(entry.id) ? .on : .off
            cell.disclosure.image = NSImage(systemSymbolName: expanded.contains(entry.id) ? "chevron.down" : "chevron.right", accessibilityDescription: nil)
            cell.disclosure.setAccessibilityLabel("\(expanded.contains(entry.id) ? "Collapse" : "Expand") \(entry.name)")
            let image: String
            switch entry.fileType {
            case "swift": image = "swift"
            case "cpp", "html": image = "chevron.left.forwardslash.chevron.right"
            case "json": image = "curlybraces"
            case "css": image = "paintpalette"
            default: image = "doc.text"
            }
            cell.icon.image = NSImage(systemSymbolName: entry.isDirectory ? "folder.fill" : image, accessibilityDescription: nil)
            cell.icon.contentTintColor = entry.isDirectory ? .systemBlue : .secondaryLabelColor
            cell.toggle = { [weak self] in self?.deferAction { $0.toggleFolder(entry) } }
            return cell
        }

        // The root and current native selection guard queued actions when a project
        // is replaced or several input events arrive before SwiftUI updates.
        private func deferAction(_ action: @escaping @MainActor (WorkspaceViewModel) -> Void) {
            let expectedRoot = root
            let expectedGeneration = workspace.projectGeneration
            DispatchQueue.main.async { [weak self] in
                guard let self, self.workspace.rootPath == expectedRoot,
                      self.workspace.projectGeneration == expectedGeneration else { return }
                action(self.workspace)
            }
        }

        func tableViewSelectionDidChange(_ notification: Notification) {
            guard !updating, let table, entries.indices.contains(table.selectedRow) else { return }
            let path = entries[table.selectedRow].id
            deferAction { [weak self] workspace in
                guard let self, let table = self.table,
                      self.entries.indices.contains(table.selectedRow), self.entries[table.selectedRow].id == path else { return }
                workspace.selectedEntryID = path
            }
        }

        func folderKey(expand: Bool) {
            guard let table, entries.indices.contains(table.selectedRow) else { return }
            let entry = entries[table.selectedRow]
            if entry.isDirectory && workspace.expandedFolders.contains(entry.id) != expand {
                deferAction { $0.toggleFolder(entry) }
            } else if expand, entry.isDirectory, table.selectedRow + 1 < entries.count {
                table.selectRowIndexes(IndexSet(integer: table.selectedRow + 1), byExtendingSelection: false)
            } else if !expand,
                      let parent = entries.firstIndex(where: { $0.path == (entry.path as NSString).deletingLastPathComponent }) {
                table.selectRowIndexes(IndexSet(integer: parent), byExtendingSelection: false)
            }
        }
    }
}

private struct CodeSnippetView: NSViewRepresentable {
    let text: String
    let language: String

    final class Coordinator {
        var text: String?
        var language: String?
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> NSScrollView {
        let scrollView = NSScrollView()
        scrollView.drawsBackground = false
        scrollView.borderType = .noBorder
        scrollView.hasVerticalScroller = true
        scrollView.hasHorizontalScroller = true
        scrollView.autohidesScrollers = true

        let textView = NSTextView()
        textView.isEditable = false
        textView.isSelectable = true
        textView.drawsBackground = false
        textView.textContainerInset = NSSize(width: 14, height: 14)
        textView.usesFindBar = true
        textView.allowsUndo = false
        textView.isRichText = true
        textView.importsGraphics = false
        textView.usesFontPanel = false
        textView.usesRuler = false
        textView.isHorizontallyResizable = true
        textView.isVerticallyResizable = true
        textView.autoresizingMask = [.width]
        textView.minSize = NSSize(width: 0, height: 0)
        textView.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        textView.textContainer?.widthTracksTextView = true
        textView.textContainer?.containerSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        textView.textContainer?.lineFragmentPadding = 0
        textView.setContentHuggingPriority(.defaultLow, for: .horizontal)
        textView.setContentHuggingPriority(.defaultLow, for: .vertical)

        scrollView.documentView = textView
        update(textView: textView)
        context.coordinator.text = text
        context.coordinator.language = language
        return scrollView
    }

    func updateNSView(_ nsView: NSScrollView, context: Context) {
        guard context.coordinator.text != text || context.coordinator.language != language else { return }
        guard let textView = nsView.documentView as? NSTextView else {
            return
        }
        update(textView: textView)
        context.coordinator.text = text
        context.coordinator.language = language
    }

    private func update(textView: NSTextView) {
        let attributed = SyntaxHighlighter.highlight(text: text, language: language)
        textView.textStorage?.setAttributedString(attributed)
        textView.scroll(NSPoint(x: 0, y: 0))
    }
}

@MainActor
private enum SyntaxHighlighter {
    private static let baseFont = NSFont.monospacedSystemFont(ofSize: 13, weight: .regular)
    private static let baseColor = NSColor(calibratedRed: 0.88, green: 0.90, blue: 0.93, alpha: 1)
    private static let keywordColor = NSColor(calibratedRed: 0.99, green: 0.62, blue: 0.31, alpha: 1)
    private static let typeColor = NSColor(calibratedRed: 0.54, green: 0.76, blue: 0.98, alpha: 1)
    private static let stringColor = NSColor(calibratedRed: 0.56, green: 0.84, blue: 0.56, alpha: 1)
    private static let commentColor = NSColor(calibratedRed: 0.52, green: 0.57, blue: 0.63, alpha: 1)
    private static let numberColor = NSColor(calibratedRed: 0.78, green: 0.67, blue: 0.97, alpha: 1)
    private static let punctuationColor = NSColor(calibratedRed: 0.92, green: 0.94, blue: 0.96, alpha: 1)

    static func highlight(text: String, language: String) -> NSAttributedString {
        let attributed = NSMutableAttributedString(
            string: text,
            attributes: [
                .font: baseFont,
                .foregroundColor: baseColor
            ]
        )

        let nsRange = NSRange(text.startIndex..<text.endIndex, in: text)
        let patterns = patternsForLanguage(language.lowercased())
        for rule in patterns {
            guard let regex = try? NSRegularExpression(pattern: rule.pattern, options: rule.options) else {
                continue
            }
            regex.enumerateMatches(in: text, options: [], range: nsRange) { match, _, _ in
                guard let match else { return }
                attributed.addAttributes(rule.attributes, range: match.range)
            }
        }

        return attributed
    }

    private static func patternsForLanguage(_ language: String) -> [HighlightRule] {
        let shared: [HighlightRule] = [
            HighlightRule(pattern: "\"(?:\\\\.|[^\"\\\\])*\"", attributes: [.foregroundColor: stringColor]),
            HighlightRule(pattern: "\\b\\d+(?:\\.\\d+)?\\b", attributes: [.foregroundColor: numberColor]),
        ]

        switch language {
        case "swift":
            return shared + [
                HighlightRule(pattern: "//.*", attributes: [.foregroundColor: commentColor]),
                HighlightRule(pattern: "/\\*[\\s\\S]*?\\*/", options: [.dotMatchesLineSeparators], attributes: [.foregroundColor: commentColor]),
                HighlightRule(pattern: "\\b(import|struct|class|actor|enum|protocol|extension|func|var|let|if|else|guard|switch|case|for|while|return|throws|throw|async|await|try|catch|where|in|init|deinit|public|private|internal|fileprivate|open|static)\\b", attributes: [.foregroundColor: keywordColor]),
                HighlightRule(pattern: "\\b[A-Z][A-Za-z0-9_]*\\b", attributes: [.foregroundColor: typeColor]),
            ]
        case "json":
            return [
                HighlightRule(pattern: "\"(?:\\\\.|[^\"\\\\])*\"(?=\\s*:)", attributes: [.foregroundColor: typeColor]),
                HighlightRule(pattern: "\"(?:\\\\.|[^\"\\\\])*\"", attributes: [.foregroundColor: stringColor]),
                HighlightRule(pattern: "\\b(true|false|null)\\b", attributes: [.foregroundColor: keywordColor]),
                HighlightRule(pattern: "\\b-?\\d+(?:\\.\\d+)?(?:[eE][+-]?\\d+)?\\b", attributes: [.foregroundColor: numberColor]),
                HighlightRule(pattern: "[\\{\\}\\[\\]:,]", attributes: [.foregroundColor: punctuationColor]),
            ]
        case "script", "ts", "javascript", "typescript", "tsx", "jsx", "css", "scss", "less", "html", "cpp", "c", "objc", "php", "rust", "python", "java", "csharp", "go", "kotlin", "ruby", "shell", "qml":
            return shared + [
                HighlightRule(pattern: "//.*", attributes: [.foregroundColor: commentColor]),
                HighlightRule(pattern: "/\\*[\\s\\S]*?\\*/", options: [.dotMatchesLineSeparators], attributes: [.foregroundColor: commentColor]),
                HighlightRule(pattern: "#.*", attributes: [.foregroundColor: commentColor]),
                HighlightRule(pattern: "\\b(function|class|const|let|var|import|export|if|else|switch|case|for|while|return|async|await|public|private|protected|static|def|fn|struct|enum|impl|namespace|using|new|try|catch)\\b", attributes: [.foregroundColor: keywordColor]),
                HighlightRule(pattern: "\\b[A-Z][A-Za-z0-9_]*\\b", attributes: [.foregroundColor: typeColor]),
            ]
        default:
            return shared
        }
    }

    private struct HighlightRule {
        let pattern: String
        var options: NSRegularExpression.Options = []
        let attributes: [NSAttributedString.Key: Any]
    }
}
