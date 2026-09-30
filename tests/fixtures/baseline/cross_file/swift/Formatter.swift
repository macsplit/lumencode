struct PriceFormatter {
    func format(cents: Int) -> String {
        return "\(cents / 100)"
    }
}

func formatReceiptLine(label: String, cents: Int) -> String {
    return label + ": " + PriceFormatter().format(cents: cents)
}
