import Foundation
import CryptoKit
import Darwin

public enum NativeBridgeError: Error, LocalizedError {
    case helperMissing, cancelled, timedOut, invalidResponse, outputTooLarge
    case failed(Int32)
    case runtimeRequiresMacOS(String)

    public var errorDescription: String? {
        switch self {
        case .helperMissing: "Analysis engine is missing. Run python3 tools/build_engine.py and package EngineRuntime with the app."
        case .cancelled: "Analysis cancelled."
        case .timedOut: "Analysis engine exceeded its time limit."
        case .failed(let status): "Analysis engine exited with status \(status)."
        case .runtimeRequiresMacOS(let version): "This analysis runtime requires macOS \(version). Rebuild it with dependencies compatible with this Mac."
        case .invalidResponse: "Analysis engine returned invalid JSON."
        case .outputTooLarge: "Analysis engine returned too much data."
        }
    }
}

/// Cancellation stops the process doing the work, including requests cancelled before launch.
public final class AnalysisRequest: @unchecked Sendable {
    private let lock = NSLock()
    private var process: Process?
    private var cancellation: NativeBridgeError?
    public init() {}
    public func cancel() { stop(.cancelled) }

    fileprivate func stop(_ reason: NativeBridgeError) {
        lock.lock()
        defer { lock.unlock() }
        guard cancellation == nil else { return }
        cancellation = reason
        if let process, process.isRunning {
            process.terminate()
            DispatchQueue.global().asyncAfter(deadline: .now() + 0.25) { self.killIfRunning() }
        }
    }

    private func killIfRunning() {
        lock.lock()
        defer { lock.unlock() }
        if let process, process.isRunning { kill(process.processIdentifier, SIGKILL) }
    }

    fileprivate func start(_ process: Process) throws {
        lock.lock()
        defer { lock.unlock() }
        if let cancellation { throw cancellation }
        self.process = process
        try process.run()
    }

    fileprivate func finish() throws {
        lock.lock()
        defer { lock.unlock() }
        process = nil
        if let cancellation { throw cancellation }
    }
}

/// Runs the original parser/index in a bounded helper process, never inside the GUI.
public struct NativeAnalyzer: Sendable {
    private let executable: URL?
    private let timeLimit: TimeInterval?
    public init(helperURL: URL? = nil, timeLimit: TimeInterval? = nil) {
        executable = helperURL
        self.timeLimit = timeLimit
    }

    public static var helperURL: URL? {
        let manager = FileManager.default
        if let override = ProcessInfo.processInfo.environment["LUMENCODE_ENGINE"] {
            let url = URL(fileURLWithPath: override)
            return manager.isExecutableFile(atPath: url.path) ? url : nil
        }
        var candidates: [URL] = []
        if let resources = Bundle.main.resourceURL {
            let bundled = resources.appendingPathComponent("EngineRuntime/bin/lumencode-engine")
            if Bundle.main.bundleURL.pathExtension == "app" {
                return manager.isExecutableFile(atPath: bundled.path) ? bundled : nil
            }
            candidates.append(bundled)
        }
        let executable = URL(fileURLWithPath: CommandLine.arguments[0]).standardizedFileURL.deletingLastPathComponent()
        candidates.append(executable.appendingPathComponent("EngineRuntime/bin/lumencode-engine"))
        // SwiftPM runs tests from the package directory, including when the build
        // output is outside the checkout. Search its parents for the root runtime.
        let workingDirectory = URL(fileURLWithPath: manager.currentDirectoryPath)
        for start in [executable, workingDirectory] {
            var ancestor = start
            for _ in 0..<6 {
                candidates.append(ancestor.appendingPathComponent(".build/EngineRuntime/bin/lumencode-engine"))
                ancestor.deleteLastPathComponent()
            }
        }
        return candidates.first { manager.isExecutableFile(atPath: $0.path) }
    }

    public func analyzeFile(path: String, rootPath: String? = nil, request: AnalysisRequest = AnalysisRequest()) throws -> FileAnalysis {
        let arguments = rootPath.map { ["--root", $0, "--path", path] } ?? ["--dump-file", path]
        return try EnginePayloadAdapter.decode(run(arguments: arguments, timeout: rootPath == nil ? 5 : 30, request: request))
    }

    public func buildIndex(rootPath: String, request: AnalysisRequest) throws {
        _ = try run(arguments: ["--root", rootPath, "--build-index"], timeout: 60, request: request)
    }

    public func search(query: String, rootPath: String, kinds: String = "", request: AnalysisRequest = AnalysisRequest()) throws -> [SymbolSearchResult] {
        let data = try run(arguments: ["--root", rootPath, "--search", query, "--kind", kinds], timeout: 30, request: request)
        struct Response: Decodable { let results: [SymbolSearchResult] }
        return try JSONDecoder().decode(Response.self, from: data).results
    }

    private func run(arguments: [String], timeout: TimeInterval, request: AnalysisRequest) throws -> Data {
        guard let helper = executable ?? Self.helperURL else { throw NativeBridgeError.helperMissing }
        let requirements = helper.deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent("minimum-macos.json")
        if let data = try? Data(contentsOf: requirements),
           let versions = try? JSONDecoder().decode([String: String].self, from: data) {
            let parsed = versions.values.map { value -> [Int] in
                let components = value.split(separator: ".").compactMap { Int($0) }
                return Array((components + [0, 0, 0]).prefix(3))
            }
            if let newest = parsed.max(by: { $0.lexicographicallyPrecedes($1) }),
               !ProcessInfo.processInfo.isOperatingSystemAtLeast(OperatingSystemVersion(majorVersion: newest[0], minorVersion: newest[1], patchVersion: newest[2])) {
                throw NativeBridgeError.runtimeRequiresMacOS(newest.map(String.init).joined(separator: "."))
            }
        }
        let process = Process()
        process.executableURL = helper
        process.arguments = arguments
        let output = Pipe()
        process.standardOutput = output
        process.standardError = FileHandle.nullDevice
        var environment = ProcessInfo.processInfo.environment
        environment.removeValue(forKey: "DYLD_LIBRARY_PATH")
        environment.removeValue(forKey: "DYLD_FRAMEWORK_PATH")
        process.environment = environment
        try request.start(process)
        let deadline = DispatchWorkItem { request.stop(.timedOut) }
        DispatchQueue.global().asyncAfter(deadline: .now() + (timeLimit ?? timeout), execute: deadline)
        defer { deadline.cancel() }
        var data = Data()
        while let chunk = try output.fileHandleForReading.read(upToCount: 64 * 1024), !chunk.isEmpty {
            data.append(chunk)
            if data.count > 32 * 1024 * 1024 {
                request.stop(.outputTooLarge)
                break
            }
        }
        process.waitUntilExit()
        try request.finish()
        guard process.terminationReason == .exit, process.terminationStatus == 0 else {
            throw NativeBridgeError.failed(process.terminationStatus)
        }
        guard !data.isEmpty else { throw NativeBridgeError.invalidResponse }
        return data
    }
}

public struct SymbolSearchResult: Codable, Identifiable, Sendable {
    public let name: String
    public let kind: String
    public let path: String
    public let line: Int
    public let language: String
    public let score: Int
    public var id: String { "\(path)|\(kind)|\(name)|\(line)" }
}

/// A presentation adapter only. Upstream JSON is retained for lossless CLI output.
enum EnginePayloadAdapter {
    static func decode(_ data: Data) throws -> FileAnalysis {
        guard var payload = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              payload["path"] is String, payload["symbols"] is [Any] else {
            throw NativeBridgeError.invalidResponse
        }
        let path = payload["path"] as? String ?? ""
        let language = payload["language"] as? String ?? ""
        payload["symbols"] = (payload["symbols"] as? [[String: Any]] ?? []).map { symbol($0, path: path, language: language) }
        for collection in ["quickLinks", "dependencies", "routes", "relatedFiles"] {
            payload[collection] = (payload[collection] as? [[String: Any]] ?? []).map { item($0, collection: collection, path: path) }
        }
        var package = defaults(payload["packageSummary"] as? [String: Any] ?? [:], ["name": "", "version": "", "main": "", "dependencies": [], "scripts": [:]])
        package["dependencies"] = (package["dependencies"] as? [Any] ?? []).compactMap { value -> String? in
            if let name = value as? String { return name }
            return (value as? [String: Any])?["name"] as? String
        }
        payload["packageSummary"] = package
        var css = defaults(payload["cssSummary"] as? [String: Any] ?? [:], ["usedClasses": [], "availableClasses": [], "matchedClasses": [], "missingClasses": []])
        for field in ["matchedClasses", "missingClasses"] {
            css[field] = (css[field] as? [[String: Any]] ?? []).map { value in
                defaults(value, ["id": identity(path + "|" + field + "|" + (value["name"] as? String ?? "")), "kind": "class", "matched": field == "matchedClasses", "path": "", "line": 0, "snippet": ""])
            }
        }
        payload["cssSummary"] = css
        let confidence = payload["analysisConfidence"] as? String ?? "low"
        payload["analysisConfidenceLabel"] = confidence
        payload["analysisConfidence"] = ["high": 1.0, "medium": 0.7, "low": 0.3][confidence] ?? 0.0
        payload["analysisDamagedLines"] = payload["analysisDamagedLines"] ?? []
        payload["analysisHasAstErrors"] = payload["analysisHasAstErrors"] ?? false
        payload["analysisPartial"] = payload["analysisPartial"] ?? false
        payload["analysisNotices"] = (payload["analysisNotices"] as? [[String: Any]] ?? []).enumerated().map { index, value in
            defaults(value, ["id": identity(path + "|notice|\(index)"), "severity": "warning", "message": "", "line": 0, "column": 0])
        }
        payload["callEdges"] = []
        let normalized = try JSONSerialization.data(withJSONObject: payload)
        var analysis = try JSONDecoder().decode(FileAnalysis.self, from: normalized)
        analysis.engineJSON = data
        return analysis
    }

    private static func defaults(_ value: [String: Any], _ fallback: [String: Any]) -> [String: Any] {
        fallback.merging(value) { fallback, current in current is NSNull ? fallback : current }
    }

    private static func identity(_ value: String) -> String {
        let bytes = Array(SHA256.hash(data: Data(value.utf8)).prefix(16))
        return NSUUID(uuidBytes: bytes).uuidString
    }

    private static func symbol(_ value: [String: Any], path: String, language: String) -> [String: Any] {
        let sourcePath = value["path"] as? String ?? value["sourcePath"] as? String ?? path
        var result = defaults(value, ["id": identity(sourcePath + "|\(value["kind"] ?? "")|\(value["name"] ?? "")|\(value["line"] ?? 0)"), "kind": "", "name": "", "line": 0, "detail": "", "snippet": "", "skipDiagnostics": false, "path": sourcePath, "language": language])
        result["members"] = (value["members"] as? [[String: Any]] ?? []).map { symbol($0, path: sourcePath, language: language) }
        for field in ["calls", "calledBy"] {
            result[field] = (value[field] as? [[String: Any]] ?? []).map { item($0, collection: field, path: sourcePath) }
        }
        let parameters = (value["parameters"] as? [[String: Any]] ?? []).map { parameter in
            defaults(parameter.merging(["defaultValue": parameter["default"] ?? NSNull()]) { _, new in new }, ["name": "", "isVariadic": false, "isOptional": false])
        }
        let returns = (value["returns"] as? [[String: Any]] ?? []).compactMap { $0["text"] as? String }
        result["signature"] = ["parameters": parameters, "returnType": returns.isEmpty ? NSNull() : returns.joined(separator: " | ") as Any, "isAsync": false, "isThrows": false, "typeParameters": []]
        return result
    }

    private static func item(_ value: [String: Any], collection: String, path: String) -> [String: Any] {
        let key = path + "|" + collection + "|\(value["path"] ?? "")|\(value["name"] ?? value["label"] ?? "")|\(value["line"] ?? 0)"
        var result = defaults(value, ["id": identity(key), "type": "", "kind": "", "name": "", "label": "", "target": value["path"] ?? "", "line": 0, "snippet": "", "detail": "", "exists": false, "method": ""])
        if collection == "quickLinks", let target = value["targetPath"] as? String, !target.isEmpty {
            result["target"] = target
        }
        return result
    }
}

indirect enum EngineJSONValue: Codable {
    case object([String: EngineJSONValue]), array([EngineJSONValue]), string(String), number(Double), bool(Bool), null
    init(from decoder: any Decoder) throws {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() { self = .null }
        else if let value = try? container.decode(Bool.self) { self = .bool(value) }
        else if let value = try? container.decode(String.self) { self = .string(value) }
        else if let value = try? container.decode(Double.self) { self = .number(value) }
        else if let value = try? container.decode([String: EngineJSONValue].self) { self = .object(value) }
        else { self = .array(try container.decode([EngineJSONValue].self)) }
    }
    func encode(to encoder: any Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .object(let value): try container.encode(value)
        case .array(let value): try container.encode(value)
        case .string(let value): try container.encode(value)
        case .number(let value): try container.encode(value)
        case .bool(let value): try container.encode(value)
        case .null: try container.encodeNil()
        }
    }
}
