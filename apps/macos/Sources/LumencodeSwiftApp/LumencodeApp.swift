import AppKit
import SwiftUI
import LumencodeCore

@main
struct LumencodeApp: App {
    @StateObject private var workspace = WorkspaceViewModel()

    var body: some Scene {
        WindowGroup {
            ContentView(workspace: workspace)
                .frame(minWidth: 1080, minHeight: 560)
        }
        .defaultSize(width: 1240, height: 680)
        .commands {
            CommandGroup(after: .newItem) {
                Button("Open Folder…") {
                    workspace.openFolder()
                }
                .keyboardShortcut("o")
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
    @Published var expandedFolders: Set<String> = []
    @Published var isLoading = false
    @Published var loadingProgress: Double = 0.0

    private let scanner = ProjectScanner()
    private let analyzer = NativeAnalyzer()
    private var projectIndex: ProjectIndex?
    private var allAnalyses: [String: FileAnalysis] = [:]

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
        isLoading = true
        loadingProgress = 0.0

        let absolutePath = WorkspaceSupport.resolvePath(path, relativeTo: FileManager.default.currentDirectoryPath)
        rootPath = absolutePath
        errorMessage = ""
        statusMessage = "Scanning \(absolutePath)"

        // Run heavy work on background queue to keep UI responsive
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }

            let scanResult = self.scanner.scan(rootPath: absolutePath, selectedPath: nil)

            DispatchQueue.main.async {
                self.scanResult = scanResult
                self.statusMessage = "Analyzing project…"
                self.loadingProgress = 0.1
            }

            // Analyze project - this is the heavy lifting
            let projectURL = URL(fileURLWithPath: absolutePath)
            let projectAnalyzer = ProjectAnalyzer(projectRoot: projectURL)

            let (analyses, index) = projectAnalyzer.analyzeProject(
                fileEntries: scanResult.entries,
                progress: { percent in
                    DispatchQueue.main.async {
                        self.loadingProgress = 0.1 + (percent * 0.8)
                        self.statusMessage = "Analyzing project… \(Int(percent * 100))%"
                    }
                }
            )

            // Update UI on main thread
            DispatchQueue.main.async {
                self.projectIndex = index
                self.allAnalyses = Dictionary(uniqueKeysWithValues: analyses.map { ($0.path, $0) })
                self.loadingProgress = 0.95

                if let selectedPath = scanResult.selectedPath.isEmpty ? self.entries.first?.id : scanResult.selectedPath {
                    self.selectedEntryID = selectedPath
                } else {
                    self.statusMessage = "No supported files were found in this folder."
                    self.selectedAnalysis = FileAnalysis.skeleton(path: "", fileName: "", language: "")
                    self.selectedSnippet = .empty
                    self.detailTitle = "Inspector"
                    self.detailSubtitle = "Open a folder with supported source files."
                    self.detailMembers = []
                    self.detailCalls = []
                    self.detailCalledBy = []
                    self.detailCrossFileCalls = []
                    self.detailCrossFileCalledBy = []
                }

                self.loadingProgress = 1.0
                self.isLoading = false
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
        inspectionSelection = .relation(relation.id)
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
        // Navigate to the target file
        selectedEntryID = edge.to

        // After selection updates, find and inspect the target symbol
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
            if let targetSymbol = self.selectedAnalysis.symbols.first(where: { $0.name == edge.toName }) {
                self.inspectSymbol(targetSymbol)
            }
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
        guard let entry = selectedEntry else {
            return
        }

        errorMessage = ""

        if entry.isDirectory {
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

        do {
            // Use pre-analyzed data from project analyzer if available
            if let preAnalyzed = allAnalyses[entry.path] {
                selectedAnalysis = preAnalyzed
            } else {
                // Fallback: analyze on-demand if not in project analysis
                let rawAnalysis = try analyzer.analyzeFile(path: entry.path)
                selectedAnalysis = rawAnalysis
            }
            resetToFileContext()
            statusMessage = "Analyzed \(entry.name)"
        } catch {
            selectedAnalysis = FileAnalysis.skeleton(
                path: entry.path,
                fileName: entry.name,
                language: entry.fileType
            )
            selectedAnalysis.summary = "Analysis failed: \(error)"
            resetToFileContext()
            errorMessage = "Failed to analyze \(entry.name)."
        }
    }
}

struct ContentView: View {
    @ObservedObject var workspace: WorkspaceViewModel

    var body: some View {
        VSplitView {
            HSplitView {
                sidebar
                    .frame(minWidth: 280, idealWidth: 320, maxWidth: 420)
                overviewPane
                    .frame(minWidth: 340, idealWidth: 420)
                detailPane
                    .frame(minWidth: 320, idealWidth: 400)
            }
            codePane
                .frame(minHeight: 220, idealHeight: 280)
        }
        .background(Color(nsColor: .windowBackgroundColor))
        .preferredColorScheme(.dark)
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
        .overlay(alignment: .center) {
            if workspace.isLoading {
                VStack(spacing: 20) {
                    ProgressView(value: workspace.loadingProgress)
                        .frame(width: 200)
                    Text(workspace.statusMessage)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .padding(30)
                .background(.regularMaterial)
                .clipShape(RoundedRectangle(cornerRadius: 16))
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

            if workspace.entries.isEmpty {
                Spacer()
                ContentUnavailableView(
                    "No Project Open",
                    systemImage: "folder",
                    description: Text("Choose a folder to start exploring.")
                )
                Spacer()
            } else {
                List(selection: $workspace.selectedEntryID) {
                    ForEach(visibleEntries) { entry in
                        HStack(spacing: 8) {
                            if entry.isDirectory {
                                Button(action: {
                                    if workspace.expandedFolders.contains(entry.id) {
                                        workspace.expandedFolders.remove(entry.id)
                                    } else {
                                        workspace.expandedFolders.insert(entry.id)
                                    }
                                }) {
                                    Image(systemName: workspace.expandedFolders.contains(entry.id) ? "triangle.fill" : "triangle")
                                        .font(.caption)
                                        .frame(width: 10)
                                }
                                .buttonStyle(.plain)
                                Image(systemName: "folder.fill")
                                    .foregroundStyle(.blue)
                            } else {
                                Image(systemName: iconName(for: entry.fileType))
                                    .foregroundStyle(.secondary)
                            }
                            Text(entry.name)
                                .lineLimit(1)
                        }
                        .padding(.leading, CGFloat(entry.depth) * 12)
                        .tag(entry.id)
                    }
                }
                .listStyle(.sidebar)
                .scrollContentBackground(.hidden)
                .background(Color.clear)
            }
        }
        .padding(16)
        .background(shellPanel)
    }

    private var visibleEntries: [FileTreeEntry] {
        workspace.entries.filter { entry in
            if entry.depth == 0 {
                return true
            }
            var parent = entry
            while parent.depth > 0 {
                guard let parentEntry = workspace.entries.first(where: { $0.path == (parent.path as NSString).deletingLastPathComponent }) else {
                    return false
                }
                if !workspace.expandedFolders.contains(parentEntry.id) {
                    return false
                }
                parent = parentEntry
            }
            return true
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

    @ViewBuilder
    private var overviewSection: some View {
        if let entry = workspace.selectedEntry, !entry.isDirectory {
            if !workspace.selectedAnalysis.symbols.isEmpty {
                sectionCard(title: "Symbols") {
                    ForEach(workspace.selectedAnalysis.symbols) { symbol in
                        VStack(alignment: .leading, spacing: 8) {
                            selectionRow(
                                title: symbol.name,
                                subtitle: symbolSubtitle(for: symbol),
                                actionTitle: "Inspect"
                            ) {
                                workspace.inspectSymbol(symbol)
                            }

                            if !symbol.members.isEmpty {
                                ForEach(symbol.members) { member in
                                    HStack(spacing: 12) {
                                        Rectangle()
                                            .fill(Color.secondary.opacity(0.3))
                                            .frame(width: 2)
                                        selectionRow(
                                            title: member.name,
                                            subtitle: symbolSubtitle(for: member),
                                            actionTitle: "Inspect"
                                        ) {
                                            workspace.inspectMember(member)
                                        }
                                    }
                                    .padding(.leading, 16)
                                }
                            }
                        }
                    }
                }
            } else {
                headerCard(title: "No Symbols", subtitle: "The current analyzer did not find structured symbols for this file yet.")
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

    private func iconName(for fileType: String) -> String {
        switch fileType {
        case "swift":
            return "swift"
        case "cpp":
            return "chevron.left.forwardslash.chevron.right"
        case "json":
            return "curlybraces"
        case "html":
            return "chevron.left.slash.chevron.right"
        case "css":
            return "paintpalette"
        case "php", "code":
            return "doc.plaintext"
        default:
            return "doc"
        }
    }
}

private struct CodeSnippetView: NSViewRepresentable {
    let text: String
    let language: String

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
        return scrollView
    }

    func updateNSView(_ nsView: NSScrollView, context: Context) {
        guard let textView = nsView.documentView as? NSTextView else {
            return
        }
        update(textView: textView)
    }

    private func update(textView: NSTextView) {
        let attributed = SyntaxHighlighter.highlight(text: text, language: language)
        if textView.textStorage?.string != text || textView.textStorage?.length != attributed.length {
            textView.textStorage?.setAttributedString(attributed)
        } else {
            textView.textStorage?.setAttributedString(attributed)
        }
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
        case "javascript", "typescript", "tsx", "jsx", "css", "html", "cpp", "c", "php", "rust", "python", "java", "csharp":
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
