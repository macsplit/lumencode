import Foundation
import LumencodeCore

struct CLI {
    let arguments: [String]
    let scanner = ProjectScanner()
    let analyzer = NativeAnalyzer()

    func run() throws {
        if let dumpFilePath = optionValue(named: "--dump-file") {
            // Upstream's dump contract preserves an absolute input path, including
            // symlink spelling. GUI workspace identity still uses resolved paths.
            let path = NSString(string: dumpFilePath).isAbsolutePath ? dumpFilePath : resolvedPath(dumpFilePath)
            let analysis = try analyzer.analyzeFile(path: path)
            try printJSON(analysis)
            return
        }

        let rootPath = rootArgument() ?? FileManager.default.currentDirectoryPath
        let absoluteRootPath = resolvedPath(rootPath)
        let selectedPath = optionValue(named: "--path").map(resolvedPath)

        if let query = optionValue(named: "--search") {
            try printJSON(analyzer.search(query: query, rootPath: absoluteRootPath, kinds: optionValue(named: "--kind") ?? ""))
            return
        }

        let scan = scanner.scan(rootPath: absoluteRootPath, selectedPath: selectedPath)
        let analysis = scan.selectedPath.isEmpty
            ? FileAnalysis.skeleton(path: "", fileName: "", language: "")
            : try analyzer.analyzeFile(path: scan.selectedPath, rootPath: absoluteRootPath)

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
        var skipValue = false
        for argument in arguments.dropFirst() {
            if skipValue { skipValue = false; continue }
            if ["--path", "--dump-file", "--search", "--kind"].contains(argument) { skipValue = true; continue }
            if !argument.hasPrefix("-") { return argument }
        }
        return nil
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
