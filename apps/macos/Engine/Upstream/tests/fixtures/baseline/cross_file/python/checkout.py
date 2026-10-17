from pricing import apply_discount, TaxTable


def checkout_total(total, region):
    table = TaxTable()
    discounted = apply_discount(total, 0.1)
    return discounted * (1 + table.rate_for(region))
