import Foundation

public enum ProjectRelationships {
    public static func augmentSelectedAnalysis(
        _ analysis: FileAnalysis,
        rootPath: String,
        entries: [FileTreeEntry],
        analyzer: NativeAnalyzer
    ) -> FileAnalysis {
        guard isScriptLike(analysis.language), !analysis.path.isEmpty else {
            return analysis
        }

        let candidatePaths = entries
            .filter { !$0.isDirectory && isScriptLike($0.fileType) }
            .map(\.path)

        var cache: [String: FileAnalysis] = [analysis.path: analysis]

        func loadAnalysis(for path: String) -> FileAnalysis? {
            if let cached = cache[path] {
                return cached
            }
            guard let loaded = try? analyzer.analyzeFile(path: path) else {
                return nil
            }
            cache[path] = loaded
            return loaded
        }

        let importedAnalyses = analysis.dependencies
            .filter { $0.type == "import" || $0.type == "require" }
            .compactMap(\.path)
            .filter { !$0.isEmpty }
            .compactMap(loadAnalysis(for:))

        let incomingAnalyses = candidatePaths
            .filter { $0 != analysis.path }
            .compactMap(loadAnalysis(for:))
            .filter { candidate in
                candidate.dependencies.contains { dependency in
                    dependency.path == analysis.path
                }
            }

        let importedSymbols = importedAnalyses.reduce(into: [String: SymbolRelationItem]()) { partial, candidate in
            for (name, relation) in symbolRelations(from: candidate) {
                partial[name] = relation
            }
        }
        let incomingSymbols = incomingAnalyses.flatMap { candidate in
            symbolRelations(from: candidate).values.map { (candidate.path, $0) }
        }

        var augmented = analysis
        augmented.symbols = augmented.symbols.map { symbol in
            augmentSymbol(symbol, importedSymbols: importedSymbols, incomingSymbols: incomingSymbols)
        }
        return augmented
    }

    private static func augmentSymbol(
        _ symbol: SymbolItem,
        importedSymbols: [String: SymbolRelationItem],
        incomingSymbols: [(String, SymbolRelationItem)]
    ) -> SymbolItem {
        var updated = symbol
        updated.members = updated.members.map { augmentSymbol($0, importedSymbols: importedSymbols, incomingSymbols: incomingSymbols) }

        let localCallNames = Set(updated.calls.map(\.name))
        let localCallerNames = Set(updated.calledBy.map(\.name))
        let outgoingNames = relationNames(from: updated.snippet)

        for name in outgoingNames.sorted() {
            guard localCallNames.contains(name) == false, let target = importedSymbols[name] else {
                continue
            }
            updated.calls.append(target.with(detail: "calls imported symbol"))
        }

        for (_, relation) in incomingSymbols where relationNames(from: relation.snippet).contains(updated.name) {
            guard localCallerNames.contains(relation.name) == false else {
                continue
            }
            updated.calledBy.append(relation.with(detail: "called by imported usage"))
        }

        return updated
    }

    private static func symbolRelations(from analysis: FileAnalysis) -> [String: SymbolRelationItem] {
        var relations: [String: SymbolRelationItem] = [:]
        for symbol in flattenedSymbols(analysis.symbols) {
            relations[symbol.name] = SymbolRelationItem(
                kind: symbol.kind,
                name: symbol.name,
                line: symbol.line,
                detail: symbol.detail,
                snippet: symbol.snippet,
                path: symbol.path ?? analysis.path,
                language: symbol.language ?? analysis.language,
                snippetKind: symbol.snippetKind,
                diagnosticsMode: symbol.diagnosticsMode
            )
        }
        return relations
    }

    private static func flattenedSymbols(_ symbols: [SymbolItem]) -> [SymbolItem] {
        symbols.flatMap { symbol in
            [symbol] + flattenedSymbols(symbol.members)
        }
    }

    private static func relationNames(from snippet: String) -> Set<String> {
        let patterns = [
            #"\b([A-Za-z_][A-Za-z0-9_]*)\s*\("#,
            #"\.\s*([A-Za-z_][A-Za-z0-9_]*)\s*\("#
        ]
        var names: Set<String> = []
        for pattern in patterns {
            guard let regex = try? NSRegularExpression(pattern: pattern) else {
                continue
            }
            let range = NSRange(snippet.startIndex..., in: snippet)
            for match in regex.matches(in: snippet, range: range) {
                guard match.numberOfRanges > 1, let capture = Range(match.range(at: 1), in: snippet) else {
                    continue
                }
                names.insert(String(snippet[capture]))
            }
        }
        return names
    }

    private static func isScriptLike(_ language: String) -> Bool {
        ["script", "jsx", "ts", "tsx", "code"].contains(language)
    }
}

private extension SymbolRelationItem {
    func with(detail: String) -> SymbolRelationItem {
        SymbolRelationItem(
            kind: kind,
            name: name,
            line: line,
            detail: detail,
            snippet: snippet,
            path: path,
            language: language,
            snippetKind: snippetKind,
            diagnosticsMode: diagnosticsMode
        )
    }
}
