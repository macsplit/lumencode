import Foundation

public struct FileTreeEntry: Identifiable, Hashable, Codable, Sendable {
    public let id: String
    public let name: String
    public let path: String
    public let isDirectory: Bool
    public let depth: Int
    public let expanded: Bool
    public let hasChildren: Bool
    public let fileType: String
}

public struct SymbolItem: Identifiable, Hashable, Codable, Sendable {
    public let id: UUID
    public var kind: String
    public var name: String
    public var line: Int
    public var detail: String
    public var members: [SymbolItem]
    public var snippet: String
    public var path: String?
    public var language: String?
    public var sourcePath: String?
    public var sourceLanguage: String?
    public var snippetKind: String?
    public var diagnosticsMode: String?
    public var skipDiagnostics: Bool
    public var calls: [SymbolRelationItem]
    public var calledBy: [SymbolRelationItem]

    public init(
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
        skipDiagnostics: Bool = false,
        calls: [SymbolRelationItem] = [],
        calledBy: [SymbolRelationItem] = []
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
        self.calls = calls
        self.calledBy = calledBy
    }
}

public struct SymbolRelationItem: Identifiable, Hashable, Codable, Sendable {
    public let id: UUID
    public var kind: String
    public var name: String
    public var line: Int
    public var detail: String
    public var snippet: String
    public var path: String?
    public var language: String?
    public var snippetKind: String?
    public var diagnosticsMode: String?

    public init(
        id: UUID = UUID(),
        kind: String,
        name: String,
        line: Int = 0,
        detail: String = "",
        snippet: String = "",
        path: String? = nil,
        language: String? = nil,
        snippetKind: String? = nil,
        diagnosticsMode: String? = nil
    ) {
        self.id = id
        self.kind = kind
        self.name = name
        self.line = line
        self.detail = detail
        self.snippet = snippet
        self.path = path
        self.language = language
        self.snippetKind = snippetKind
        self.diagnosticsMode = diagnosticsMode
    }
}

public struct DependencyItem: Identifiable, Hashable, Codable, Sendable {
    public let id: UUID
    public var type: String
    public var label: String
    public var target: String
    public var line: Int
    public var snippet: String
    public var detail: String
    public var path: String?
    public var exists: Bool
    public var snippetKind: String?
    public var diagnosticsMode: String?

    public init(
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

public struct RouteItem: Identifiable, Hashable, Codable, Sendable {
    public let id: UUID
    public var method: String
    public var path: String
    public var line: Int
    public var label: String
    public var snippet: String
    public var detail: String
    public var snippetKind: String?
    public var diagnosticsMode: String?

    public init(
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

public struct RelatedFileItem: Identifiable, Hashable, Codable, Sendable {
    public let id: UUID
    public var type: String
    public var label: String
    public var path: String
    public var exists: Bool

    public init(id: UUID = UUID(), type: String, label: String, path: String, exists: Bool) {
        self.id = id
        self.type = type
        self.label = label
        self.path = path
        self.exists = exists
    }
}

public struct QuickLinkItem: Identifiable, Hashable, Codable, Sendable {
    public let id: UUID
    public var type: String
    public var label: String
    public var target: String
    public var exists: Bool
    public var line: Int
    public var snippet: String
    public var detail: String

    public init(
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

public struct PackageSummary: Hashable, Codable, Sendable {
    public var name: String = ""
    public var version: String = ""
    public var main: String = ""
    public var dependencies: [String] = []
    public var scripts: [String: String] = [:]

    public init() {}
}

public struct CSSClassItem: Identifiable, Hashable, Codable, Sendable {
    public let id: UUID
    public var name: String
    public var kind: String
    public var matched: Bool
    public var path: String
    public var line: Int
    public var snippet: String

    public init(
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

public struct CSSSummary: Hashable, Codable, Sendable {
    public var usedClasses: [String] = []
    public var availableClasses: [String] = []
    public var matchedClasses: [CSSClassItem] = []
    public var missingClasses: [CSSClassItem] = []

    public init() {}
}

public struct FileAnalysis: Hashable, Codable, Sendable {
    public var path: String
    public var fileName: String
    public var language: String
    public var symbols: [SymbolItem]
    public var quickLinks: [QuickLinkItem]
    public var dependencies: [DependencyItem]
    public var routes: [RouteItem]
    public var relatedFiles: [RelatedFileItem]
    public var packageSummary: PackageSummary
    public var cssSummary: CSSSummary
    public var summary: String

    public init(
        path: String,
        fileName: String,
        language: String,
        symbols: [SymbolItem],
        quickLinks: [QuickLinkItem],
        dependencies: [DependencyItem],
        routes: [RouteItem],
        relatedFiles: [RelatedFileItem],
        packageSummary: PackageSummary,
        cssSummary: CSSSummary,
        summary: String
    ) {
        self.path = path
        self.fileName = fileName
        self.language = language
        self.symbols = symbols
        self.quickLinks = quickLinks
        self.dependencies = dependencies
        self.routes = routes
        self.relatedFiles = relatedFiles
        self.packageSummary = packageSummary
        self.cssSummary = cssSummary
        self.summary = summary
    }

    public static func skeleton(path: String, fileName: String, language: String) -> FileAnalysis {
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

public struct DiagnosticItem: Identifiable, Hashable, Codable, Sendable {
    public let id: UUID
    public var severity: String
    public var message: String
    public var line: Int
    public var column: Int

    public init(id: UUID = UUID(), severity: String, message: String, line: Int = 0, column: Int = 0) {
        self.id = id
        self.severity = severity
        self.message = message
        self.line = line
        self.column = column
    }
}

public struct SnippetPayload: Hashable, Codable, Sendable {
    public var kind: String
    public var name: String
    public var path: String
    public var language: String
    public var line: Int
    public var snippet: String
    public var detail: String
    public var snippetKind: String
    public var diagnosticsMode: String
    public var isTruncated: Bool
    public var displaySnippet: String
    public var diagnostics: [DiagnosticItem]

    public init(
        kind: String,
        name: String,
        path: String,
        language: String,
        line: Int,
        snippet: String,
        detail: String,
        snippetKind: String,
        diagnosticsMode: String,
        isTruncated: Bool,
        displaySnippet: String,
        diagnostics: [DiagnosticItem]
    ) {
        self.kind = kind
        self.name = name
        self.path = path
        self.language = language
        self.line = line
        self.snippet = snippet
        self.detail = detail
        self.snippetKind = snippetKind
        self.diagnosticsMode = diagnosticsMode
        self.isTruncated = isTruncated
        self.displaySnippet = displaySnippet
        self.diagnostics = diagnostics
    }

    public static let empty = SnippetPayload(
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

public struct ProjectSummary: Hashable, Codable, Sendable {
    public var fileTypeCounts: [String: Int]
    public var totalFiles: Int
    public var totalDirectories: Int
    public var mainEntryPoint: String

    public init(fileTypeCounts: [String: Int], totalFiles: Int, totalDirectories: Int, mainEntryPoint: String) {
        self.fileTypeCounts = fileTypeCounts
        self.totalFiles = totalFiles
        self.totalDirectories = totalDirectories
        self.mainEntryPoint = mainEntryPoint
    }

    public static let empty = ProjectSummary(
        fileTypeCounts: [:],
        totalFiles: 0,
        totalDirectories: 0,
        mainEntryPoint: ""
    )
}

public struct CLIState: Codable, Sendable {
    public var rootPath: String
    public var projectSummary: ProjectSummary
    public var selectedPath: String
    public var selectedRelativePath: String
    public var selectedFileData: FileAnalysis
    public var selectedSnippet: SnippetPayload
    public var fileSystem: [FileTreeEntry]

    public init(
        rootPath: String,
        projectSummary: ProjectSummary,
        selectedPath: String,
        selectedRelativePath: String,
        selectedFileData: FileAnalysis,
        selectedSnippet: SnippetPayload,
        fileSystem: [FileTreeEntry]
    ) {
        self.rootPath = rootPath
        self.projectSummary = projectSummary
        self.selectedPath = selectedPath
        self.selectedRelativePath = selectedRelativePath
        self.selectedFileData = selectedFileData
        self.selectedSnippet = selectedSnippet
        self.fileSystem = fileSystem
    }
}
