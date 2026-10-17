func printReceipt(total: Int) {
    let line = formatReceiptLine(label: "Total", cents: total)
    print(line)
}
