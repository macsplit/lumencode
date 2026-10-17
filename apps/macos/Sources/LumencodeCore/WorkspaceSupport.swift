import Foundation
import Darwin

public enum WorkspaceSupport {
    public static func resolvePath(_ path: String, relativeTo basePath: String) -> String {
        let url = URL(fileURLWithPath: path, relativeTo: URL(fileURLWithPath: basePath))
        let normalized = url.standardizedFileURL.resolvingSymlinksInPath().path
        // Match FileManager enumeration's physical spelling. Foundation may
        // otherwise return /tmp for a directory enumerated as /private/tmp.
        return normalized.withCString { pointer in
            guard let physical = realpath(pointer, nil) else { return normalized }
            defer { free(physical) }
            return String(cString: physical)
        }
    }

    public static func relativePath(_ path: String, from rootPath: String) -> String {
        guard !path.isEmpty else {
            return ""
        }
        func stripRoot(_ file: String, _ root: String) -> String? {
            if file == root { return "" }
            let prefix = root.hasSuffix("/") ? root : root + "/"
            return file.hasPrefix(prefix) ? String(file.dropFirst(prefix.count)) : nil
        }
        if let relative = stripRoot(path, rootPath) { return relative }
        // Foundation and filesystem enumeration may use different spellings of
        // /tmp and /private/tmp. Compare canonical paths, never replace a prefix
        // found in the middle of an absolute path (which produced /privatefile.js).
        let root = resolvePath(rootPath, relativeTo: FileManager.default.currentDirectoryPath)
        let file = resolvePath(path, relativeTo: FileManager.default.currentDirectoryPath)
        return stripRoot(file, root) ?? file
    }

    public static func snippetPayload(from analysis: FileAnalysis) -> SnippetPayload {
        if !analysis.summary.hasPrefix("Analysis skipped"),
           let preview = filePreviewPayload(path: analysis.path, name: analysis.fileName, language: analysis.language) {
            return preview
        }

        if !analysis.summary.isEmpty {
            return snippetPayload(
                kind: "file",
                name: analysis.fileName,
                path: analysis.path,
                language: analysis.language,
                line: 0,
                snippet: analysis.summary,
                detail: "",
                snippetKind: "context_excerpt",
                diagnosticsMode: "none"
            )
        }

        return .empty
    }

    public static func snippetPayload(
        from symbol: SymbolItem,
        fallbackPath: String,
        fallbackLanguage: String
    ) -> SnippetPayload {
        snippetPayload(
            kind: symbol.kind,
            name: symbol.name,
            path: symbol.path ?? fallbackPath,
            language: symbol.language ?? fallbackLanguage,
            line: symbol.line,
            snippet: symbol.snippet,
            detail: symbol.detail,
            snippetKind: symbol.snippetKind ?? "line_excerpt",
            diagnosticsMode: symbol.diagnosticsMode ?? "none"
        )
    }

    public static func snippetPayload(
        from dependency: DependencyItem,
        fallbackPath: String,
        fallbackLanguage: String
    ) -> SnippetPayload {
        snippetPayload(
            kind: "dependency",
            name: dependency.label,
            path: dependency.path ?? fallbackPath,
            language: fallbackLanguage,
            line: dependency.line,
            snippet: dependency.snippet,
            detail: dependency.detail.isEmpty ? "\(dependency.type) dependency" : dependency.detail,
            snippetKind: dependency.snippetKind ?? "line_excerpt",
            diagnosticsMode: dependency.diagnosticsMode ?? "none"
        )
    }

    public static func snippetPayload(
        from route: RouteItem,
        fallbackPath: String,
        fallbackLanguage: String
    ) -> SnippetPayload {
        snippetPayload(
            kind: "route",
            name: route.label.isEmpty ? "\(route.method) \(route.path)" : route.label,
            path: fallbackPath,
            language: fallbackLanguage,
            line: route.line,
            snippet: route.snippet,
            detail: route.detail.isEmpty ? "route" : route.detail,
            snippetKind: route.snippetKind ?? "line_excerpt",
            diagnosticsMode: route.diagnosticsMode ?? "none"
        )
    }

    public static func snippetPayload(
        from quickLink: QuickLinkItem,
        fallbackPath: String,
        fallbackLanguage: String
    ) -> SnippetPayload {
        let resolvedPath = quickLink.sourcePath ?? fallbackPath
        let resolvedLanguage = quickLink.sourceLanguage ?? fallbackLanguage

        return snippetPayload(
            kind: quickLink.type,
            name: quickLink.label,
            path: resolvedPath,
            language: resolvedLanguage,
            line: quickLink.line,
            snippet: quickLink.snippet,
            detail: quickLink.detail.isEmpty ? quickLink.target : quickLink.detail,
            snippetKind: "line_excerpt",
            diagnosticsMode: "none"
        )
    }

    public static func snippetPayload(
        from relation: SymbolRelationItem,
        fallbackPath: String,
        fallbackLanguage: String
    ) -> SnippetPayload {
        snippetPayload(
            kind: relation.kind,
            name: relation.name,
            path: relation.path ?? fallbackPath,
            language: relation.language ?? fallbackLanguage,
            line: relation.line,
            snippet: relation.snippet,
            detail: relation.detail,
            snippetKind: relation.snippetKind ?? "line_excerpt",
            diagnosticsMode: relation.diagnosticsMode ?? "none"
        )
    }

    public static func filePreviewPayload(path: String, name: String, language: String, maxLines: Int = 28) -> SnippetPayload? {
        guard !path.isEmpty else {
            return nil
        }
        guard let handle = try? FileHandle(forReadingFrom: URL(fileURLWithPath: path)) else {
            return nil
        }
        defer { try? handle.close() }
        guard let data = try? handle.read(upToCount: 32 * 1024), !data.isEmpty else {
            return nil
        }
        let text = String(decoding: data.prefix(16 * 1024), as: UTF8.self)

        let lines = text.split(separator: "\n", omittingEmptySubsequences: false)
        let previewLines = lines.prefix(maxLines).map(String.init)
        var preview = previewLines.joined(separator: "\n")
        let truncated = lines.count > maxLines || data.count >= 16 * 1024
        if truncated {
            preview += "\n..."
        }

        return SnippetPayload(
            kind: "file",
            name: name,
            path: path,
            language: language,
            line: 1,
            snippet: preview,
            detail: "",
            snippetKind: "file_preview",
            diagnosticsMode: "none",
            isTruncated: truncated,
            displaySnippet: preview,
            diagnostics: []
        )
    }

    public static func snippetPayload(
        kind: String,
        name: String,
        path: String,
        language: String,
        line: Int,
        snippet: String,
        detail: String,
        snippetKind: String,
        diagnosticsMode: String
    ) -> SnippetPayload {
        let display = snippet.isEmpty ? "No snippet available." : snippet
        return SnippetPayload(
            kind: kind,
            name: name,
            path: path,
            language: language,
            line: line,
            snippet: display,
            detail: detail,
            snippetKind: snippetKind,
            diagnosticsMode: diagnosticsMode,
            isTruncated: snippet.contains("\n...") || snippet.hasSuffix("..."),
            displaySnippet: display,
            diagnostics: []
        )
    }

    public static func languageForPath(_ path: String) -> String {
        switch URL(fileURLWithPath: path).pathExtension.lowercased() {
        case "swift":
            return "swift"
        case "js", "mjs", "cjs":
            return "script"
        case "jsx":
            return "jsx"
        case "ts":
            return "ts"
        case "tsx":
            return "tsx"
        case "py":
            return "python"
        case "rs":
            return "rust"
        case "php":
            return "php"
        case "java":
            return "java"
        case "cs":
            return "csharp"
        case "cpp", "cc", "cxx", "hpp", "h":
            return "cpp"
        case "css", "scss", "sass", "less":
            return "css"
        case "html", "htm":
            return "html"
        case "json":
            return "json"
        default:
            return ""
        }
    }
}
