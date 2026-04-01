import Foundation

struct ProjectScanResult {
    var entries: [FileTreeEntry]
    var projectSummary: ProjectSummary
    var selectedPath: String
}

struct ProjectScanner {
    private let maxDepth = 64
    private let maxNodes = 50_000
    private let maxEntriesPerDirectory = 2_000

    func scan(rootPath: String, selectedPath: String?) -> ProjectScanResult {
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
            entries: entries.sorted { lhs, rhs in
                if lhs.depth != rhs.depth {
                    return lhs.depth < rhs.depth
                }
                if lhs.path.deletingLastPathComponent != rhs.path.deletingLastPathComponent {
                    return lhs.path < rhs.path
                }
                if lhs.isDirectory != rhs.isDirectory {
                    return lhs.isDirectory && !rhs.isDirectory
                }
                return lhs.name.localizedCaseInsensitiveCompare(rhs.name) == .orderedAscending
            },
            projectSummary: ProjectSummary(
                fileTypeCounts: fileTypeCounts,
                totalFiles: totalFiles,
                totalDirectories: totalDirectories,
                mainEntryPoint: chooseMainEntryPoint(rootPath: rootPath, candidateFiles: candidateFiles)
            ),
            selectedPath: chosenPath
        )
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
            "Sources/LumencodeSwift/main.swift",
            "Package.swift",
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

        let candidatesByRelative = Dictionary(uniqueKeysWithValues: candidateFiles.map {
            (relativePathString(path: $0, rootPath: rootPath).lowercased(), $0)
        })

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
        if relative.contains("/test/") || relative.contains("/tests/") { result -= 200 }
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
            "dist", "build", "coverage", "vendor", "DerivedData"
        ])
        return ignored.contains(name)
    }

    private func shouldIncludeFile(url: URL) -> Bool {
        let supported = Set([
            "swift", "js", "jsx", "ts", "tsx", "php", "html", "css", "json", "py",
            "c", "cc", "cpp", "cxx", "h", "hpp", "java", "cs", "rs", "m", "mm"
        ])
        return supported.contains(url.pathExtension.lowercased())
    }

    private func detectFileType(url: URL, isDirectory: Bool) -> String {
        if isDirectory {
            return "folder"
        }
        switch url.pathExtension.lowercased() {
        case "swift":
            return "swift"
        case "js", "jsx", "ts", "tsx":
            return "code"
        case "php":
            return "php"
        case "html":
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
        case "c", "cc", "cpp", "cxx", "h", "hpp", "m", "mm":
            return "cpp"
        default:
            return "file"
        }
    }
}

private func relativePathString(path: String, rootPath: String) -> String {
    URL(fileURLWithPath: path).path.replacingOccurrences(of: URL(fileURLWithPath: rootPath).path + "/", with: "")
}

private extension String {
    var deletingLastPathComponent: String {
        (self as NSString).deletingLastPathComponent
    }
}
