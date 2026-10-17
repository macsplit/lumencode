pub struct Tokenizer {
    input: String,
}

impl Tokenizer {
    pub fn new(input: &str) -> Tokenizer {
        Tokenizer { input: input.to_string() }
    }
}

pub fn count_tokens(input: &str) -> usize {
    input.split_whitespace().count()
}
