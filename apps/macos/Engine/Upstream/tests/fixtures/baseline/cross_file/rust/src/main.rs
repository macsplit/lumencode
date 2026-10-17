mod parser;

use parser::{count_tokens, Tokenizer};

fn run(text: &str) -> usize {
    let _tokenizer = Tokenizer::new(text);
    count_tokens(text)
}

fn main() {
    println!("{}", run("a b c"));
}
