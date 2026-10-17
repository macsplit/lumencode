function helper() {
    return 1;
}

function wrapper() {
    return helper();
}

module.exports = {
    helper,
    wrapper
};
