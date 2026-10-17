import Foundation

public struct ProjectScanResult: Sendable {
    public var entries: [FileTreeEntry]
    public var projectSummary: ProjectSummary
    public var selectedPath: String

    public init(entries: [FileTreeEntry], projectSummary: ProjectSummary, selectedPath: String) {
        self.entries = entries
        self.projectSummary = projectSummary
        self.selectedPath = selectedPath
    }
}

public struct ProjectScanner {
    private let maxDepth = 64
    private let maxNodes = 50_000
    private let maxEntriesPerDirectory = 2_000

    public init() {}

    public func scan(rootPath requestedRoot: String, selectedPath: String?) -> ProjectScanResult {
        // Enumeration yields physical paths on macOS (e.g. /private/tmp).
        // Resolve the root once rather than canonicalizing both paths per entry.
        let rootPath = WorkspaceSupport.resolvePath(requestedRoot, relativeTo: FileManager.default.currentDirectoryPath)
        let rootURL = URL(fileURLWithPath: rootPath)
        let fileManager = FileManager.default

        guard let enumerator = fileManager.enumerator(
            at: rootURL,
            includingPropertiesForKeys: [.isDirectoryKey, .isRegularFileKey],
            options: [.skipsHiddenFiles],
            errorHandler: { _, _ in true }
        ) else {
            return ProjectScanResult(entries: [], projectSummary: .empty, selectedPath: selectedPath ?? "")
        }

        var entries: [FileTreeEntry] = []
        var fileTypeCounts: [String: Int] = [:]
        var totalFiles = 0
        var totalDirectories = 0
        var nodeCount = 0
        var childrenPerDirectory: [String: Int] = [:]
        var candidateFiles: [String] = []

        while let item = enumerator.nextObject() as? URL {
            nodeCount += 1
            if nodeCount > maxNodes {
                break
            }

            let relativePath = relativePathString(path: item.path, rootPath: rootPath)
            let depth = relativePath.isEmpty ? 0 : relativePath.split(separator: "/").count

            if depth > maxDepth {
                enumerator.skipDescendants()
                continue
            }

            let parentPath = item.deletingLastPathComponent().path
            let count = childrenPerDirectory[parentPath, default: 0]
            if count >= maxEntriesPerDirectory {
                if isDirectory(at: item) {
                    enumerator.skipDescendants()
                }
                continue
            }
            childrenPerDirectory[parentPath] = count + 1

            let isDirectory = isDirectory(at: item)
            let name = item.lastPathComponent

            if isDirectory && shouldIgnoreDirectory(name) {
                enumerator.skipDescendants()
                continue
            }

            if !isDirectory && !shouldIncludeFile(url: item) {
                continue
            }

            let fileType = detectFileType(url: item, isDirectory: isDirectory)
            entries.append(
                FileTreeEntry(
                    id: item.path,
                    name: name,
                    path: item.path,
                    isDirectory: isDirectory,
                    depth: max(depth - 1, 0),
                    expanded: false,
                    hasChildren: isDirectory,
                    fileType: fileType
                )
            )

            if isDirectory {
                totalDirectories += 1
            } else {
                totalFiles += 1
                fileTypeCounts[fileType, default: 0] += 1
                candidateFiles.append(item.path)
            }
        }

        let chosenPath = resolveSelectedPath(
            rootPath: rootPath,
            selectedPath: selectedPath,
            candidateFiles: candidateFiles
        )

        return ProjectScanResult(
            entries: depthFirstOrder(entries),
            projectSummary: ProjectSummary(
                fileTypeCounts: fileTypeCounts,
                totalFiles: totalFiles,
                totalDirectories: totalDirectories,
                mainEntryPoint: chooseMainEntryPoint(rootPath: rootPath, candidateFiles: candidateFiles)
            ),
            selectedPath: chosenPath
        )
    }

    // Tree views rely on each folder being immediately followed by its descendants.
    private func depthFirstOrder(_ entries: [FileTreeEntry]) -> [FileTreeEntry] {
        let childrenByParent = Dictionary(grouping: entries) { $0.path.deletingLastPathComponent }
        let topLevelParents = Set(entries.filter { $0.depth == 0 }.map { $0.path.deletingLastPathComponent })

        var ordered: [FileTreeEntry] = []
        ordered.reserveCapacity(entries.count)

        func append(childrenOf parent: String) {
            let children = (childrenByParent[parent] ?? []).sorted { lhs, rhs in
                if lhs.isDirectory != rhs.isDirectory {
                    return lhs.isDirectory
                }
                return lhs.name.localizedCaseInsensitiveCompare(rhs.name) == .orderedAscending
            }
            for child in children {
                ordered.append(child)
                if child.isDirectory {
                    append(childrenOf: child.path)
                }
            }
        }

        for parent in topLevelParents.sorted() {
            append(childrenOf: parent)
        }
        return ordered
    }

    private func resolveSelectedPath(rootPath: String, selectedPath: String?, candidateFiles: [String]) -> String {
        if let selectedPath, FileManager.default.fileExists(atPath: selectedPath) {
            return selectedPath
        }
        let mainEntryPoint = chooseMainEntryPoint(rootPath: rootPath, candidateFiles: candidateFiles)
        if !mainEntryPoint.isEmpty {
            return mainEntryPoint
        }
        return candidateFiles.sorted().first ?? ""
    }

    private func chooseMainEntryPoint(rootPath: String, candidateFiles: [String]) -> String {
        let preferred = [
            "Sources/LumencodeSwiftApp/LumencodeApp.swift",
            "Sources/LumencodeSwift/main.swift",
            "Package.swift",
            "src/main.cpp", "src/main.cc", "src/main.cxx", "src/main.c",
            "src/main.tsx", "src/main.ts", "src/main.jsx", "src/main.js",
            "src/index.tsx", "src/index.ts", "src/index.jsx", "src/index.js",
            "main.ts", "main.js", "app.ts", "app.js", "index.ts", "index.js",
            "main.py", "app.py", "Program.cs", "program.cs", "src/main.rs",
            "src/main.py", "src/main.java", "index.html"
        ]

        let packageEntry = packageMainEntry(rootPath: rootPath)
        if !packageEntry.isEmpty {
            return packageEntry
        }

        let candidatesByRelative = candidateFiles.sorted().reduce(into: [String: String]()) { values, path in
            let key = relativePathString(path: path, rootPath: rootPath).lowercased()
            if values[key] == nil { values[key] = path }
        }

        for relative in preferred {
            if let path = candidatesByRelative[relative.lowercased()] {
                return path
            }
        }

        return candidateFiles.sorted { score(path: $0, rootPath: rootPath) > score(path: $1, rootPath: rootPath) }.first ?? ""
    }

    private func packageMainEntry(rootPath: String) -> String {
        let packageURL = URL(fileURLWithPath: rootPath).appendingPathComponent("package.json")
        guard
            let data = try? Data(contentsOf: packageURL),
            let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else {
            return ""
        }

        for key in ["browser", "module", "main"] {
            if let value = json[key] as? String {
                let resolved = URL(fileURLWithPath: rootPath).appendingPathComponent(value).standardizedFileURL.path
                if FileManager.default.fileExists(atPath: resolved) {
                    return resolved
                }
            }
        }

        return ""
    }

    private func score(path: String, rootPath: String) -> Int {
        let relative = relativePathString(path: path, rootPath: rootPath).lowercased()
        let fileName = URL(fileURLWithPath: path).lastPathComponent.lowercased()

        var result = 0
        if relative.hasPrefix("src/") { result += 80 }
        if !relative.contains("/") { result += 20 }
        if relative.hasPrefix("test/") || relative.hasPrefix("tests/") || relative.contains("/test/") || relative.contains("/tests/") { result -= 200 }
        if relative.hasPrefix("third_party/") || relative.hasPrefix("vendor/") || relative.hasPrefix("engine/upstream/") { result -= 500 }
        if relative.contains("/dist/") || relative.contains("/build/") { result -= 150 }
        if fileName == "main.ts" || fileName == "main.js" || fileName == "main.tsx" || fileName == "main.jsx" { result += 120 }
        if fileName == "main.swift" { result += 120 }
        if fileName == "package.swift" { result += 100 }
        if fileName == "index.ts" || fileName == "index.js" || fileName == "index.tsx" || fileName == "index.jsx" { result += 80 }
        if fileName == "app.ts" || fileName == "app.js" || fileName == "app.py" { result += 70 }
        result -= relative.split(separator: "/").count * 5
        return result
    }

    private func isDirectory(at url: URL) -> Bool {
        (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false
    }

    private func shouldIgnoreDirectory(_ name: String) -> Bool {
        let ignored = Set([
            ".git", ".build", ".swiftpm", ".next", ".nuxt", "node_modules",
            "dist", "build", "coverage", "vendor", "deriveddata", "__pycache__", "venv", ".venv"
        ])
        return ignored.contains(name.lowercased())
    }

    private func shouldIncludeFile(url: URL) -> Bool {
        let supported = Set([
            "swift", "js", "jsx", "ts", "tsx", "php", "html", "css", "json", "py",
            "c", "cc", "cpp", "cxx", "h", "hpp", "hh", "hxx", "java", "cs", "rs", "m", "mm",
            "mjs", "cjs", "mts", "cts", "htm", "qml", "go", "vb", "sql", "sh", "bash", "zsh",
            "rb", "rake", "gemspec", "ru", "kt", "kts", "scss", "less",
            "aspx", "ascx", "master", "ashx", "asmx", "asax"
        ])
        return supported.contains(url.pathExtension.lowercased()) || ["Gemfile", "Rakefile"].contains(url.lastPathComponent)
    }

    private func detectFileType(url: URL, isDirectory: Bool) -> String {
        if isDirectory {
            return "folder"
        }
        switch url.pathExtension.lowercased() {
        case "swift":
            return "swift"
        case "js", "jsx", "ts", "tsx", "mjs", "cjs", "mts", "cts":
            return "code"
        case "php":
            return "php"
        case "html", "htm", "aspx", "ascx", "master", "ashx", "asmx", "asax":
            return "html"
        case "css":
            return "css"
        case "json":
            return "json"
        case "py":
            return "python"
        case "java":
            return "java"
        case "cs":
            return "csharp"
        case "rs":
            return "rust"
        case "m", "mm":
            return "objc"
        case "c", "cc", "cpp", "cxx", "h", "hpp", "hh", "hxx":
            return "cpp"
        case "qml", "go", "sql", "scss", "less":
            return url.pathExtension.lowercased()
        case "vb": return "vbnet"
        case "sh", "bash", "zsh": return "shell"
        case "kt", "kts": return "kotlin"
        case "rb", "rake", "gemspec", "ru": return "ruby"
        default:
            return ["Gemfile", "Rakefile"].contains(url.lastPathComponent) ? "ruby" : "file"
        }
    }
}

private func relativePathString(path: String, rootPath: String) -> String {
    WorkspaceSupport.relativePath(path, from: rootPath)
}

private extension String {
    var deletingLastPathComponent: String {
        (self as NSString).deletingLastPathComponent
    }
}
