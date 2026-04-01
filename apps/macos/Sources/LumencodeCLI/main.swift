import Foundation
import LumencodeCore

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
        let rawAnalysis = scan.selectedPath.isEmpty
            ? FileAnalysis.skeleton(path: "", fileName: "", language: "")
            : (try? analyzer.analyzeFile(path: scan.selectedPath)) ?? FileAnalysis.skeleton(
                path: scan.selectedPath,
                fileName: URL(fileURLWithPath: scan.selectedPath).lastPathComponent,
                language: ""
            )
        let analysis = ProjectRelationships.augmentSelectedAnalysis(
            rawAnalysis,
            rootPath: absoluteRootPath,
            entries: scan.entries,
            analyzer: analyzer
        )

        let state = CLIState(
            rootPath: absoluteRootPath,
            projectSummary: scan.projectSummary,
            selectedPath: scan.selectedPath,
            selectedRelativePath: relativePath(scan.selectedPath, from: absoluteRootPath),
            selectedFileData: analysis,
            selectedSnippet: WorkspaceSupport.snippetPayload(from: analysis),
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
        WorkspaceSupport.resolvePath(path, relativeTo: FileManager.default.currentDirectoryPath)
    }

    private func relativePath(_ path: String, from rootPath: String) -> String {
        WorkspaceSupport.relativePath(path, from: rootPath)
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
