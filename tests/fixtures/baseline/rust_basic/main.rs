use std::fmt::Debug;

struct Greeter;

impl Greeter {
    fn greet(&self) -> &'static str {
        helper_rs();
        "hello"
    }
}

fn helper_rs() -> &'static str {
    "helper"
}

fn main() {
    let greeter = Greeter;
    println!("{} {}", greeter.greet(), helper_rs());
}
