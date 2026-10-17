import Foundation
import LumencodeNativeBridge

enum NativeBridgeError: Error {
    case invalidResponse
}

struct NativeAnalyzer {
    private let decoder = JSONDecoder()

    func analyzeFile(path: String) throws -> FileAnalysis {
        let rawPointer = path.withCString { pathCString in
            lumencode_analyze_file_json(pathCString)
        }

        guard let raw = rawPointer else {
            throw NativeBridgeError.invalidResponse
        }

        defer {
            lumencode_free_string(raw)
        }

        let data = Data(bytes: raw, count: strlen(raw))
        return try decoder.decode(FileAnalysis.self, from: data)
    }
}
