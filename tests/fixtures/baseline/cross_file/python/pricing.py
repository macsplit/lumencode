def apply_discount(total, rate):
    return total * (1 - rate)


class TaxTable:
    def rate_for(self, region):
        return 0.2
