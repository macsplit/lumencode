import XCTest
@testable import LumencodeCore

final class EngineIntegrationTests: XCTestCase {
    private var repo: URL {
        URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    }

    func testOriginalSwiftSignatureAndLosslessContract() throws {
        let file = repo.appendingPathComponent("Engine/Upstream/tests/fixtures/baseline/swift_signatures/Greeter.swift")
        let analysis = try NativeAnalyzer().analyzeFile(path: file.path)
        let greeter = try XCTUnwrap(analysis.symbols.first { $0.name == "Greeter" })
        let greet = try XCTUnwrap(greeter.members.first { $0.name == "greet" })
        XCTAssertEqual(greet.signature?.parameters.map(\.name), ["_ name", "times count", "_ extras"])
        XCTAssertEqual(greet.signature?.parameters[1].defaultValue, "1")
        XCTAssertEqual(greet.signature?.returnType, "[String] (async throws)")
        XCTAssertEqual(greet.sourceMode, "ast")
        XCTAssertEqual(analysis.analysisConfidenceLabel, "high")
        let emitted = try JSONSerialization.jsonObject(with: JSONEncoder().encode(analysis)) as? NSDictionary
        let original = try JSONSerialization.jsonObject(with: XCTUnwrap(analysis.engineJSON)) as? NSDictionary
        XCTAssertEqual(emitted, original, "The UI adapter must not narrow or rewrite the upstream CLI payload")
        XCTAssertEqual(analysis.symbols.first?.id, try NativeAnalyzer().analyzeFile(path: file.path).symbols.first?.id)
    }

    func testRelativePathsHandleAliasesAndOnlyStripRootPrefixes() throws {
        let directory = "/tmp/lumencode-path-test-" + UUID().uuidString
        try FileManager.default.createDirectory(atPath: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: directory) }
        let file = directory + "/café.swift"
        try "func café() {}\n".write(toFile: file, atomically: true, encoding: .utf8)
        XCTAssertEqual(WorkspaceSupport.relativePath("/private" + file, from: directory), "café.swift")
        XCTAssertEqual(WorkspaceSupport.relativePath(file, from: "/private" + directory), "café.swift")
        XCTAssertEqual(WorkspaceSupport.relativePath(file, from: directory), "café.swift")
        XCTAssertEqual(WorkspaceSupport.resolvePath(directory, relativeTo: "/"), "/private" + directory)
        XCTAssertEqual(WorkspaceSupport.relativePath(directory, from: directory), "")
        let outside = "/other" + directory + "/café.swift"
        XCTAssertEqual(WorkspaceSupport.relativePath(outside, from: directory), outside)
        let scan = ProjectScanner().scan(rootPath: "/private" + directory, selectedPath: nil)
        XCTAssertEqual(scan.entries.first(where: { !$0.isDirectory })?.depth, 0)
    }

    func testCrossFileCallsUseOriginalIndexInBothDirections() throws {
        let root = repo.appendingPathComponent("Engine/Upstream/tests/fixtures/baseline/cross_file/swift")
        let caller = try NativeAnalyzer().analyzeFile(path: root.appendingPathComponent("Receipt.swift").path, rootPath: root.path)
        let target = try NativeAnalyzer().analyzeFile(path: root.appendingPathComponent("Formatter.swift").path, rootPath: root.path)
        func flatten(_ symbols: [SymbolItem]) -> [SymbolItem] { symbols.flatMap { [$0] + flatten($0.members) } }
        XCTAssertTrue(flatten(caller.symbols).contains { !$0.calls.isEmpty })
        XCTAssertTrue(flatten(target.symbols).contains { !$0.calledBy.isEmpty })
    }

    func testOversizedFileReturnsBoundedNotice() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let file = directory.appendingPathComponent("large.js")
        try Data(repeating: 32, count: 2 * 1024 * 1024 + 1).write(to: file)
        let analysis = try NativeAnalyzer(timeLimit: 2).analyzeFile(path: file.path)
        XCTAssertTrue(analysis.symbols.isEmpty)
        XCTAssertTrue(analysis.summary.contains("skipped"))
        XCTAssertEqual(analysis.analysisConfidenceLabel, "low")
    }

    func testCancellationBeforeLaunchAndTimeoutDuringWork() throws {
        let request = AnalysisRequest()
        request.cancel()
        XCTAssertThrowsError(try NativeAnalyzer().analyzeFile(path: "/tmp/unused.swift", request: request)) {
            guard case NativeBridgeError.cancelled = $0 else { return XCTFail("Unexpected error: \($0)") }
        }
        let fake = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try "#!/bin/sh\nexec /bin/sleep 30\n".write(to: fake, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: fake.path)
        defer { try? FileManager.default.removeItem(at: fake) }
        let start = Date()
        XCTAssertThrowsError(try NativeAnalyzer(helperURL: fake, timeLimit: 0.1).analyzeFile(path: "unused")) {
            guard case NativeBridgeError.timedOut = $0 else { return XCTFail("Unexpected error: \($0)") }
        }
        XCTAssertLessThan(Date().timeIntervalSince(start), 2)
    }

    func testMalformedHelperOutputFailsExplicitly() throws {
        let fake = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try "#!/bin/sh\nprintf '{}\\n'\n".write(to: fake, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: fake.path)
        defer { try? FileManager.default.removeItem(at: fake) }
        XCTAssertThrowsError(try NativeAnalyzer(helperURL: fake).analyzeFile(path: "unused")) {
            guard case NativeBridgeError.invalidResponse = $0 else { return XCTFail("Unexpected error: \($0)") }
        }
    }

    func testScannerIncludesNewLanguagesAndModuleExtensions() throws {
        let root = repo.appendingPathComponent("Engine/Upstream/tests/fixtures/baseline")
        let result = ProjectScanner().scan(rootPath: root.path, selectedPath: nil)
        let types = Set(result.entries.filter { !$0.isDirectory }.map(\.fileType))
        XCTAssertTrue(Set(["go", "vbnet", "sql", "ruby", "kotlin", "scss", "shell", "qml", "objc"]).isSubset(of: types))
    }

    func testRankedSearchAndFileScopeUseOriginalQueries() throws {
        let root = repo.appendingPathComponent("Engine/Upstream/tests/fixtures/baseline/swift_signatures")
        let matches = try NativeAnalyzer().search(query: "greet", rootPath: root.path)
        XCTAssertTrue(matches.contains { $0.name == "Greeter.greet" && $0.line == 10 })
        XCTAssertEqual(matches.map(\.score), matches.map(\.score).sorted(by: >))
        let files = try NativeAnalyzer().search(query: "greet", rootPath: root.path, kinds: "file")
        XCTAssertFalse(files.isEmpty)
        XCTAssertTrue(files.allSatisfy { $0.kind == "file" })
    }

    func testQuickLinkSeparatesNavigationTargetAndSourceContext() throws {
        let root = repo.appendingPathComponent("Engine/Upstream/tests/fixtures/baseline/web_app")
        let file = root.appendingPathComponent("index.html")
        let analysis = try NativeAnalyzer().analyzeFile(path: file.path)
        let stylesheet = try XCTUnwrap(analysis.quickLinks.first { $0.type == "stylesheet" })
        XCTAssertEqual(stylesheet.target, root.appendingPathComponent("css/style.css").path)
        let snippet = WorkspaceSupport.snippetPayload(from: stylesheet, fallbackPath: file.path, fallbackLanguage: "html")
        XCTAssertEqual(snippet.path, file.path)
        XCTAssertEqual(snippet.language, "html")
    }
}
