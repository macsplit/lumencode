func probe() {
    try! out.close()
    try? object.read()
    let result = try! source.make()
}
