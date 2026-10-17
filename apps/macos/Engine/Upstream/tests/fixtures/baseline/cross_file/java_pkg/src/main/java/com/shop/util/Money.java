package com.shop.util;

public final class Money {
    private final long cents;

    private Money(long cents) {
        this.cents = cents;
    }

    public static Money ofCents(long cents) {
        return new Money(cents);
    }

    public long cents() {
        return cents;
    }
}
