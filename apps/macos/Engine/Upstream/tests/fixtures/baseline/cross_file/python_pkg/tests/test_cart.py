import shop.payments.gateway as gw
from shop import Cart


def test_checkout():
    cart = Cart()
    assert cart.checkout("4111") == gw.charge_card("4111", "[]")
