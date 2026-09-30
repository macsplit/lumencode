package com.shop;

import com.shop.util.Money;
import org.junit.Test;

public class MoneyTest {
    @Test
    public void keepsCents() {
        Money price = Money.ofCents(250);
        org.junit.Assert.assertEquals(250, price.cents());
    }
}
