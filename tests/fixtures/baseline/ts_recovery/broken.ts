export function helperBroken(value: number) {
    return value + 1;
}

export function wrapperBroken(value: number) {
    if (value > 0) {
        return helperBroken(value);
    // Intentionally missing the closing brace for this if block.
    return helperBroken(value - 1);
}

export const runnerBroken = () => wrapperBroken(2);
