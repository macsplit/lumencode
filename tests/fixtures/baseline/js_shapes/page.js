const button = document.querySelector('#buy');

button.addEventListener('click', () => {
    checkout();
});

window.onload = function () {
    button.disabled = false;
};

function checkout() {
    return true;
}
