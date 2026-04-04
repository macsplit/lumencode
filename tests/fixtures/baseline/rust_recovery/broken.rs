fn helper_broken_rs() -> i32 {
    7
}

fn wrapper_broken_rs() -> i32 {
    helper_broken_rs(
}

struct WorkerBrokenRs {
    value: i32,
}
