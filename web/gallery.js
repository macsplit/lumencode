(() => {
    const gallery = document.querySelector('.screenshots');
    if (!gallery) return;

    const slides = Array.from(gallery.querySelectorAll('.screenshot'));
    const controls = gallery.querySelector('.gallery-controls');
    const status = gallery.querySelector('.gallery-status');
    if (slides.length < 2 || !controls || !status) return;

    let current = 0;
    const show = (index) => {
        current = (index + slides.length) % slides.length;
        slides.forEach((slide, i) => { slide.hidden = i !== current; });
        status.textContent = `${current + 1} / ${slides.length}`;
    };

    controls.querySelectorAll('button').forEach((button) => {
        button.addEventListener('click', () => show(current + Number(button.dataset.direction)));
    });
    gallery.addEventListener('keydown', (event) => {
        if (event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return;
        if (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight') return;
        event.preventDefault();
        // Keep keyboard focus visible when switching away from an image link.
        if (slides[current].contains(document.activeElement)) {
            controls.querySelector(`[data-direction="${event.key === 'ArrowRight' ? 1 : -1}"]`).focus();
        }
        show(current + (event.key === 'ArrowRight' ? 1 : -1));
    });

    gallery.setAttribute('aria-roledescription', 'carousel');
    show(0);
    controls.hidden = false;
})();
