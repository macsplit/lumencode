import Foundation

/// Builds and manages the project-wide index of cross-file relationships
public class ProjectIndexBuilder {
    private let projectRoot: URL
    private var fileAnalyses: [String: FileAnalysis] = [:]
    private var index = ProjectIndex()

    public init(projectRoot: URL) {
        self.projectRoot = projectRoot
    }

    /// Build a project index from analyzed files
    public func buildIndex(from analyses: [FileAnalysis]) -> ProjectIndex {
        // Collect all facts from all files
        for analysis in analyses {
            fileAnalyses[analysis.path] = analysis

            if let facts = analysis.indexFacts {
                index.definitions.append(contentsOf: facts.definitions)
                index.imports.append(contentsOf: facts.imports)
                index.callSites.append(contentsOf: facts.callSites)
            }
        }

        // Resolve cross-file edges
        resolveEdges()

        index.scanTime = Date()
        return index
    }

    /// Resolve call edges by matching call sites to definitions
    private func resolveEdges() {
        // Build maps for fast lookup
        var definitionsByName: [String: [IndexFact]] = [:]
        var definitionsByFile: [String: [IndexFact]] = [:]
        var importsByFile: [String: [IndexFact]] = [:]

        for def in index.definitions {
            if definitionsByName[def.name] == nil {
                definitionsByName[def.name] = []
            }
            definitionsByName[def.name]?.append(def)

            if definitionsByFile[def.file] == nil {
                definitionsByFile[def.file] = []
            }
            definitionsByFile[def.file]?.append(def)
        }

        for imp in index.imports {
            if importsByFile[imp.file] == nil {
                importsByFile[imp.file] = []
            }
            importsByFile[imp.file]?.append(imp)
        }

        // Process each call site to find its target
        for callSite in index.callSites {
            let callingFile = callSite.file
            let calledName = callSite.name

            // Strategy 1: Import-based matching (HIGH confidence)
            // If the call matches an imported name, find what it's imported from
            if let imports = importsByFile[callingFile] {
                for importFact in imports {
                    if importFact.binding == calledName {
                        // This call uses an imported name
                        // Find definitions of this import target
                        if let targets = definitionsByName[importFact.name] {
                            for target in targets {
                                let edge = CallEdge(
                                    from: callingFile,
                                    to: target.file,
                                    fromName: callSite.name,
                                    toName: target.name,
                                    confidence: "high",
                                    evidence: "import_binding",
                                    line: callSite.line
                                )
                                index.resolvedEdges.append(edge)
                            }
                        }
                    }
                }
            }

            // Strategy 2: Name-based matching (MEDIUM/LOW confidence)
            // Look for definitions with the same name
            if let targets = definitionsByName[calledName] {
                for target in targets {
                    // Skip if we already linked via import
                    let alreadyLinked = index.resolvedEdges.contains { edge in
                        edge.from == callingFile && edge.toName == target.name &&
                        edge.evidence == "import_binding"
                    }

                    if !alreadyLinked {
                        let confidence: String
                        let evidence: String

                        if target.file == callingFile {
                            // Same file - medium confidence
                            confidence = "medium"
                            evidence = "same_file"
                        } else if isSamePackage(callingFile, target.file) {
                            // Same package - medium confidence
                            confidence = "medium"
                            evidence = "same_package"
                        } else {
                            // Different files, distinctive name - low confidence
                            confidence = "low"
                            evidence = "distinctive_name"
                        }

                        let edge = CallEdge(
                            from: callingFile,
                            to: target.file,
                            fromName: callSite.name,
                            toName: target.name,
                            confidence: confidence,
                            evidence: evidence,
                            line: callSite.line
                        )
                        index.resolvedEdges.append(edge)
                    }
                }
            }
        }
    }

    /// Check if two files are in the same package/directory
    private func isSamePackage(_ file1: String, _ file2: String) -> Bool {
        let path1 = (file1 as NSString).deletingLastPathComponent
        let path2 = (file2 as NSString).deletingLastPathComponent
        return path1 == path2
    }

    /// Get cross-file call edges for a specific file
    public func callEdges(for filePath: String) -> [CallEdge] {
        return index.resolvedEdges.filter { $0.from == filePath }
    }

    /// Get cross-file called-by edges for a specific symbol
    public func calledByEdges(for symbolName: String, inFile: String) -> [CallEdge] {
        return index.resolvedEdges.filter { edge in
            edge.to == inFile && edge.toName == symbolName
        }
    }

    /// Save index to disk for caching
    public func cacheIndex(at cacheURL: URL) throws {
        let cache = IndexCache(
            projectRoot: projectRoot.path,
            buildTime: Date(),
            parserVersion: "1.0",
            fileChecksums: [:],
            index: index
        )

        let data = try JSONEncoder().encode(cache)
        try data.write(to: cacheURL)
    }

    /// Load index from cache if available and valid
    public static func loadCachedIndex(at cacheURL: URL) -> ProjectIndex? {
        guard let data = try? Data(contentsOf: cacheURL) else {
            return nil
        }

        do {
            let cache = try JSONDecoder().decode(IndexCache.self, from: data)
            return cache.index
        } catch {
            return nil
        }
    }
}

/// Cache structure for persisting the project index
public struct IndexCache: Codable {
    public var projectRoot: String
    public var buildTime: Date
    public var parserVersion: String
    public var fileChecksums: [String: Int64]
    public var index: ProjectIndex
}
