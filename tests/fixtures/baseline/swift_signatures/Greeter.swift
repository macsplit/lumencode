import Foundation

struct Greeter {
    let prefix: String

    init(prefix: String) {
        self.prefix = prefix
    }

    func greet(_ name: String, times count: Int = 1, _ extras: String...) async throws -> [String] {
        return Array(repeating: prefix + name, count: count)
    }

    func schedule(perform action: @escaping @Sendable (String) -> Void) {
        action(prefix)
    }
}
