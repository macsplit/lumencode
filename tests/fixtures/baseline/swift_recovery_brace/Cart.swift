struct Cart {
    var items: [Int] = []

    func total() -> Int {
        if items.isEmpty {
            return 0
        }
        return items.reduce(0, +)

    func clear() {
    }

    func add(_ x: Int) {
    }
}

func helper() -> Int {
    return 1
}
