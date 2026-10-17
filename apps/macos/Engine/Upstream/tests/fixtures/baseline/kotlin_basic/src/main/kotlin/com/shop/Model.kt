package com.shop

import java.time.Instant

/** A cart line. "Quoted { braces }" in docs are ignored. */
data class Line(val sku: String, val quantity: Int = 1)

enum class Status(val label: String) {
    OPEN("open"),
    PAID("paid"),
    SHIPPED("shipped");

    fun isFinal(): Boolean = this == SHIPPED
}

typealias Lines = List<Line>

class Cart(private val owner: String) {
    private val lines = mutableListOf<Line>()
    var status: Status = Status.OPEN

    fun add(sku: String, quantity: Int = 1): Cart {
        lines.add(Line(sku, quantity))
        log("added $sku (${quantity})")
        return this
    }

    fun total(prices: Map<String, Long>): Long =
        lines.sumOf { (prices[it.sku] ?: 0L) * it.quantity }

    private fun log(message: String) {
        println("[$owner] $message at ${Instant.now()}")
    }

    companion object {
        fun empty(owner: String) = Cart(owner)
    }
}

fun Cart.checkout(): Status {
    status = Status.PAID
    return status
}
