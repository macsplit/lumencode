package com.shop

import io.ktor.server.routing.*
import io.ktor.server.application.*

fun Application.shopRoutes() {
    routing {
        route("/api") {
            get("/carts/{id}") {
                val cart = Cart.empty("guest")
                call.respond(cart.checkout())
            }
            post("/carts") {
                call.respond(Cart("new").add("x"))
            }
        }
    }
}
