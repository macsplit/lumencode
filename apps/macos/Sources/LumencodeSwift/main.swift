import Foundation

struct CLI {
    let arguments: [String]
    let scanner = ProjectScanner()
    let analyzer = NativeAnalyzer()

    func run() throws {
        if let dumpFilePath = optionValue(named: "--dump-file") {
            let analysis = try analyzer.analyzeFile(path: resolvedPath(dumpFilePath))
            try printJSON(analysis)
            return
        }

        let rootPath = rootArgument() ?? FileManager.default.currentDirectoryPath
        let absoluteRootPath = resolvedPath(rootPath)
        let selectedPath = optionValue(named: "--path").map(resolvedPath)

        let scan = scanner.scan(rootPath: absoluteRootPath, selectedPath: selectedPath)
        let analysis = scan.selectedPath.isEmpty
            ? FileAnalysis.skeleton(path: "", fileName: "", language: "")
            : (try? analyzer.analyzeFile(path: scan.selectedPath)) ?? FileAnalysis.skeleton(
                path: scan.selectedPath,
                fileName: URL(fileURLWithPath: scan.selectedPath).lastPathComponent,
                language: ""
            )

        let state = CLIState(
            rootPath: absoluteRootPath,
            projectSummary: scan.projectSummary,
            selectedPath: scan.selectedPath,
            selectedRelativePath: relativePath(scan.selectedPath, from: absoluteRootPath),
            selectedFileData: analysis,
            selectedSnippet: snippetPayload(from: analysis),
            fileSystem: scan.entries
        )

        try printJSON(state)
    }

    private func rootArgument() -> String? {
        arguments.dropFirst().first { !$0.hasPrefix("-") }
    }

    private func optionValue(named name: String) -> String? {
        guard let index = arguments.firstIndex(of: name), index + 1 < arguments.count else {
            return nil
        }
        return arguments[index + 1]
    }

    private func resolvedPath(_ path: String) -> String {
        let url = URL(fileURLWithPath: path, relativeTo: URL(fileURLWithPath: FileManager.default.currentDirectoryPath))
        return url.standardizedFileURL.path
    }

    private func relativePath(_ path: String, from rootPath: String) -> String {
        guard !path.isEmpty else {
            return ""
        }
        let rootURL = URL(fileURLWithPath: rootPath)
        let fileURL = URL(fileURLWithPath: path)
        return fileURL.path.replacingOccurrences(of: rootURL.path + "/", with: "")
    }

    private func snippetPayload(from analysis: FileAnalysis) -> SnippetPayload {
        if let symbol = analysis.symbols.first, !symbol.snippet.isEmpty {
            return SnippetPayload(
                kind: symbol.kind,
                name: symbol.name,
                path: symbol.path ?? analysis.path,
                language: symbol.language ?? analysis.language,
                line: symbol.line,
                snippet: symbol.snippet,
                detail: symbol.detail,
                snippetKind: symbol.snippetKind ?? "line_excerpt",
                diagnosticsMode: symbol.diagnosticsMode ?? "none",
                isTruncated: false,
                displaySnippet: symbol.snippet,
                diagnostics: []
            )
        }

        if !analysis.summary.isEmpty {
            return SnippetPayload(
                kind: "file",
                name: analysis.fileName,
                path: analysis.path,
                language: analysis.language,
                line: 0,
                snippet: analysis.summary,
                detail: "",
                snippetKind: "context_excerpt",
                diagnosticsMode: "none",
                isTruncated: false,
                displaySnippet: analysis.summary,
                diagnostics: []
            )
        }

        return .empty
    }

    private func printJSON<T: Encodable>(_ value: T) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(value)
        FileHandle.standardOutput.write(data)
        FileHandle.standardOutput.write(Data("\n".utf8))
    }
}

do {
    try CLI(arguments: CommandLine.arguments).run()
} catch {
    FileHandle.standardError.write(Data("lumencode-swift failed: \(error)\n".utf8))
    exit(1)
}
