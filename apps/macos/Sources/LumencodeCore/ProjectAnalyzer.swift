import Foundation

/// Analyzes all files in a project and builds the cross-file index
public class ProjectAnalyzer {
    private let projectRoot: URL

    public init(projectRoot: URL) {
        self.projectRoot = projectRoot
    }

    /// Analyze a project directory and build the cross-file index
    /// Returns: (all analyses, project index)
    public func analyzeProject(
        fileEntries: [FileTreeEntry],
        progress: ((Double) -> Void)? = nil
    ) -> (analyses: [FileAnalysis], index: ProjectIndex) {
        let sourceFiles = fileEntries.filter { !$0.isDirectory && isSourceFile($0.fileType) }

        var analyses: [FileAnalysis] = []
        let totalFiles = sourceFiles.count

        // Analyze files
        let analyzer = NativeAnalyzer()
        for (index, file) in sourceFiles.enumerated() {
            if let analysis = try? analyzer.analyzeFile(path: file.path) {
                analyses.append(analysis)
            }
            let percent = Double(index + 1) / Double(totalFiles)
            progress?(percent)
        }

        // Build project index from all analyses
        let indexBuilder = ProjectIndexBuilder(projectRoot: projectRoot)
        let projectIndex = indexBuilder.buildIndex(from: analyses)

        // Augment each analysis with cross-file edges
        var augmentedAnalyses = analyses
        for i in 0..<augmentedAnalyses.count {
            let filePath = augmentedAnalyses[i].path
            augmentedAnalyses[i].callEdges = indexBuilder.callEdges(for: filePath)
        }

        return (analyses: augmentedAnalyses, index: projectIndex)
    }

    /// Check if a file type is source code (not binary, docs, etc)
    private func isSourceFile(_ fileType: String) -> Bool {
        let sourceTypes = Set([
            "swift", "typescript", "javascript", "python", "java",
            "csharp", "rust", "go", "cpp", "c", "php", "ruby", "kotlin",
            "shell", "sql", "html", "css", "json", "yaml", "toml"
        ])
        return sourceTypes.contains(fileType)
    }
}
