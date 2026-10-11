import Foundation

// MARK: - Signature Information

public struct ParameterInfo: Hashable, Codable, Sendable {
    public var name: String
    public var type: String?
    public var defaultValue: String?
    public var isVariadic: Bool = false
    public var isOptional: Bool = false

    public init(
        name: String,
        type: String? = nil,
        defaultValue: String? = nil,
        isVariadic: Bool = false,
        isOptional: Bool = false
    ) {
        self.name = name
        self.type = type
        self.defaultValue = defaultValue
        self.isVariadic = isVariadic
        self.isOptional = isOptional
    }
}

public struct SignatureInfo: Hashable, Codable, Sendable {
    public var parameters: [ParameterInfo] = []
    public var returnType: String?
    public var isAsync: Bool = false
    public var isThrows: Bool = false
    public var typeParameters: [String] = []

    public init(
        parameters: [ParameterInfo] = [],
        returnType: String? = nil,
        isAsync: Bool = false,
        isThrows: Bool = false,
        typeParameters: [String] = []
    ) {
        self.parameters = parameters
        self.returnType = returnType
        self.isAsync = isAsync
        self.isThrows = isThrows
        self.typeParameters = typeParameters
    }
}

// MARK: - Project Index Information

public struct IndexFact: Hashable, Codable, Sendable {
    public var kind: String        // "definition" | "import" | "call"
    public var name: String        // Symbol name or import target
    public var owner: String?      // Type/class that owns this definition
    public var file: String        // File path
    public var line: Int
    public var receiver: String?   // For calls: receiver object name
    public var binding: String?    // For imports: local binding

    public init(
        kind: String,
        name: String,
        owner: String? = nil,
        file: String,
        line: Int,
        receiver: String? = nil,
        binding: String? = nil
    ) {
        self.kind = kind
        self.name = name
        self.owner = owner
        self.file = file
        self.line = line
        self.receiver = receiver
        self.binding = binding
    }
}

public struct CallEdge: Identifiable, Hashable, Codable, Sendable {
    public let id: String
    public var from: String         // Calling file
    public var to: String           // Target file or definition
    public var fromName: String     // Function/method doing calling
    public var toName: String       // Function/method being called
    public var confidence: String   // "high" | "medium" | "low"
    public var evidence: String     // Why we think this link exists
    public var line: Int            // Call site line number

    public init(
        from: String,
        to: String,
        fromName: String,
        toName: String,
        confidence: String,
        evidence: String,
        line: Int
    ) {
        self.from = from
        self.to = to
        self.fromName = fromName
        self.toName = toName
        self.confidence = confidence
        self.evidence = evidence
        self.line = line
        self.id = "\(from):\(fromName)->\(to):\(toName):\(line)"
    }
}

public struct IndexFacts: Hashable, Codable, Sendable {
    public var definitions: [IndexFact] = []
    public var imports: [IndexFact] = []
    public var callSites: [IndexFact] = []
}

public struct ProjectIndex: Codable, Sendable {
    public var definitions: [IndexFact] = []
    public var imports: [IndexFact] = []
    public var callSites: [IndexFact] = []
    public var resolvedEdges: [CallEdge] = []
    public var version: String = "1.0"
    public var scanTime: Date = Date()
}

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
    public var signature: SignatureInfo?

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
        calledBy: [SymbolRelationItem] = [],
        signature: SignatureInfo? = nil
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
        self.signature = signature
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

    // Recovery metadata
    public var analysisSourceMode: String = "ast"      // "ast" | "recovered" | "heuristic"
    public var analysisConfidence: Double = 1.0        // 0.0-1.0
    public var analysisDamagedLines: [Int] = []        // 1-indexed line numbers

    // Index facts (Phase 3)
    public var indexFacts: IndexFacts?                 // Per-file facts for project index
    public var callEdges: [CallEdge] = []              // Cross-file relations

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
        summary: String,
        analysisSourceMode: String = "ast",
        analysisConfidence: Double = 1.0,
        analysisDamagedLines: [Int] = [],
        indexFacts: IndexFacts? = nil,
        callEdges: [CallEdge] = []
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
        self.analysisSourceMode = analysisSourceMode
        self.analysisConfidence = analysisConfidence
        self.analysisDamagedLines = analysisDamagedLines
        self.indexFacts = indexFacts
        self.callEdges = callEdges
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
