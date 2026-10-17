import Foundation

struct FileTreeEntry: Identifiable, Hashable, Codable {
    let id: String
    let name: String
    let path: String
    let isDirectory: Bool
    let depth: Int
    let expanded: Bool
    let hasChildren: Bool
    let fileType: String
}

struct SymbolItem: Identifiable, Hashable, Codable {
    let id: UUID
    var kind: String
    var name: String
    var line: Int
    var detail: String
    var members: [SymbolItem]
    var snippet: String
    var path: String?
    var language: String?
    var sourcePath: String?
    var sourceLanguage: String?
    var snippetKind: String?
    var diagnosticsMode: String?
    var skipDiagnostics: Bool

    init(
        id: UUID = UUID(),
        kind: String,
        name: String,
        line: Int,
        detail: String = "",
        members: [SymbolItem] = [],
        snippet: String = "",
        path: String? = nil,
        language: String? = nil,
        sourcePath: String? = nil,
        sourceLanguage: String? = nil,
        snippetKind: String? = nil,
        diagnosticsMode: String? = nil,
        skipDiagnostics: Bool = false
    ) {
        self.id = id
        self.kind = kind
        self.name = name
        self.line = line
        self.detail = detail
        self.members = members
        self.snippet = snippet
        self.path = path
        self.language = language
        self.sourcePath = sourcePath
        self.sourceLanguage = sourceLanguage
        self.snippetKind = snippetKind
        self.diagnosticsMode = diagnosticsMode
        self.skipDiagnostics = skipDiagnostics
    }
}

struct DependencyItem: Identifiable, Hashable, Codable {
    let id: UUID
    var type: String
    var label: String
    var target: String
    var line: Int
    var snippet: String
    var detail: String
    var path: String?
    var exists: Bool
    var snippetKind: String?
    var diagnosticsMode: String?

    init(
        id: UUID = UUID(),
        type: String,
        label: String,
        target: String,
        line: Int = 0,
        snippet: String = "",
        detail: String = "",
        path: String? = nil,
        exists: Bool = false,
        snippetKind: String? = nil,
        diagnosticsMode: String? = nil
    ) {
        self.id = id
        self.type = type
        self.label = label
        self.target = target
        self.line = line
        self.snippet = snippet
        self.detail = detail
        self.path = path
        self.exists = exists
        self.snippetKind = snippetKind
        self.diagnosticsMode = diagnosticsMode
    }
}

struct RouteItem: Identifiable, Hashable, Codable {
    let id: UUID
    var method: String
    var path: String
    var line: Int
    var label: String
    var snippet: String
    var detail: String
    var snippetKind: String?
    var diagnosticsMode: String?

    init(
        id: UUID = UUID(),
        method: String,
        path: String,
        line: Int = 0,
        label: String = "",
        snippet: String = "",
        detail: String = "",
        snippetKind: String? = nil,
        diagnosticsMode: String? = nil
    ) {
        self.id = id
        self.method = method
        self.path = path
        self.line = line
        self.label = label
        self.snippet = snippet
        self.detail = detail
        self.snippetKind = snippetKind
        self.diagnosticsMode = diagnosticsMode
    }
}

struct RelatedFileItem: Identifiable, Hashable, Codable {
    let id: UUID
    var type: String
    var label: String
    var path: String
    var exists: Bool

    init(id: UUID = UUID(), type: String, label: String, path: String, exists: Bool) {
        self.id = id
        self.type = type
        self.label = label
        self.path = path
        self.exists = exists
    }
}

struct QuickLinkItem: Identifiable, Hashable, Codable {
    let id: UUID
    var type: String
    var label: String
    var target: String
    var exists: Bool
    var line: Int
    var snippet: String
    var detail: String

    init(
        id: UUID = UUID(),
        type: String,
        label: String,
        target: String,
        exists: Bool,
        line: Int = 0,
        snippet: String = "",
        detail: String = ""
    ) {
        self.id = id
        self.type = type
        self.label = label
        self.target = target
        self.exists = exists
        self.line = line
        self.snippet = snippet
        self.detail = detail
    }
}

struct PackageSummary: Hashable, Codable {
    var name: String = ""
    var version: String = ""
    var main: String = ""
    var dependencies: [String] = []
    var scripts: [String: String] = [:]
}

struct CSSClassItem: Identifiable, Hashable, Codable {
    let id: UUID
    var name: String
    var kind: String
    var matched: Bool
    var path: String
    var line: Int
    var snippet: String

    init(
        id: UUID = UUID(),
        name: String,
        kind: String = "class",
        matched: Bool,
        path: String = "",
        line: Int = 0,
        snippet: String = ""
    ) {
        self.id = id
        self.name = name
        self.kind = kind
        self.matched = matched
        self.path = path
        self.line = line
        self.snippet = snippet
    }
}

struct CSSSummary: Hashable, Codable {
    var usedClasses: [String] = []
    var availableClasses: [String] = []
    var matchedClasses: [CSSClassItem] = []
    var missingClasses: [CSSClassItem] = []
}

struct FileAnalysis: Hashable, Codable {
    var path: String
    var fileName: String
    var language: String
    var symbols: [SymbolItem]
    var quickLinks: [QuickLinkItem]
    var dependencies: [DependencyItem]
    var routes: [RouteItem]
    var relatedFiles: [RelatedFileItem]
    var packageSummary: PackageSummary
    var cssSummary: CSSSummary
    var summary: String

    static func skeleton(path: String, fileName: String, language: String) -> FileAnalysis {
        FileAnalysis(
            path: path,
            fileName: fileName,
            language: language,
            symbols: [],
            quickLinks: [],
            dependencies: [],
            routes: [],
            relatedFiles: [],
            packageSummary: PackageSummary(),
            cssSummary: CSSSummary(),
            summary: ""
        )
    }
}

struct DiagnosticItem: Identifiable, Hashable, Codable {
    let id: UUID
    var severity: String
    var message: String
    var line: Int
    var column: Int

    init(id: UUID = UUID(), severity: String, message: String, line: Int = 0, column: Int = 0) {
        self.id = id
        self.severity = severity
        self.message = message
        self.line = line
        self.column = column
    }
}

struct SnippetPayload: Hashable, Codable {
    var kind: String
    var name: String
    var path: String
    var language: String
    var line: Int
    var snippet: String
    var detail: String
    var snippetKind: String
    var diagnosticsMode: String
    var isTruncated: Bool
    var displaySnippet: String
    var diagnostics: [DiagnosticItem]

    static let empty = SnippetPayload(
        kind: "file",
        name: "",
        path: "",
        language: "",
        line: 0,
        snippet: "",
        detail: "",
        snippetKind: "file_preview",
        diagnosticsMode: "none",
        isTruncated: false,
        displaySnippet: "Select a symbol to inspect its source snippet.",
        diagnostics: []
    )
}

struct ProjectSummary: Hashable, Codable {
    var fileTypeCounts: [String: Int]
    var totalFiles: Int
    var totalDirectories: Int
    var mainEntryPoint: String

    static let empty = ProjectSummary(
        fileTypeCounts: [:],
        totalFiles: 0,
        totalDirectories: 0,
        mainEntryPoint: ""
    )
}

struct CLIState: Codable {
    var rootPath: String
    var projectSummary: ProjectSummary
    var selectedPath: String
    var selectedRelativePath: String
    var selectedFileData: FileAnalysis
    var selectedSnippet: SnippetPayload
    var fileSystem: [FileTreeEntry]
}
