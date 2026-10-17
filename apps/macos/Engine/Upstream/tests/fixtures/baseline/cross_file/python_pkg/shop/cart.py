import json as serializer

from .payments import gateway


class Cart:
    def __init__(self):
        self.items = []

    def checkout(self, card):
        payload = serializer.dumps(self.items)
        return gateway.charge_card(card, payload)
