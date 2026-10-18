(() => {
    const gallery = document.querySelector('.screenshots');
    if (!gallery) return;
    const slides = Array.from(gallery.querySelectorAll('.screenshot'));
    const controls = gallery.querySelector('.gallery-controls');
    const dotsContainer = gallery.querySelector('.gallery-dots');
    const playback = gallery.querySelector('.gallery-playback');
    if (slides.length < 2 || !controls || !dotsContainer || !playback) return;

    const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)');
    let current = 0;
    let paused = reducedMotion.matches;
    let hovering = false;
    let timer;
    const schedule = () => {
        clearTimeout(timer);
        const stopped = paused || hovering || gallery.contains(document.activeElement) || document.hidden;
        gallery.querySelector('.gallery-slides').setAttribute('aria-live', stopped ? 'polite' : 'off');
        if (!stopped) timer = setTimeout(() => show(current + 1), 10000);
    };
    const dots = slides.map((slide, index) => {
        const dot = document.createElement('button');
        dot.type = 'button';
        dot.className = 'gallery-dot';
        dot.setAttribute('aria-label', `Show screenshot ${index + 1}: ${slide.querySelector('figcaption').textContent}`);
        dot.setAttribute('aria-controls', slide.id);
        dot.addEventListener('click', () => show(index));
        dotsContainer.append(dot);
        return dot;
    });
    const show = (index) => {
        current = (index + slides.length) % slides.length;
        slides.forEach((slide, i) => { slide.hidden = i !== current; });
        dots.forEach((dot, i) => dot.setAttribute('aria-current', String(i === current)));
        // Load the next image before the next automatic advance.
        slides[(current + 1) % slides.length].querySelector('img').loading = 'eager';
        schedule();
    };
    const updatePlayback = () => {
        const label = paused ? 'Start slideshow' : 'Pause slideshow';
        playback.setAttribute('aria-label', label);
        playback.title = label;
        playback.textContent = paused ? '▶' : 'Ⅱ';
        schedule();
    };
    playback.addEventListener('click', () => { paused = !paused; updatePlayback(); });
    gallery.addEventListener('mouseenter', () => { hovering = true; schedule(); });
    gallery.addEventListener('mouseleave', () => { hovering = false; schedule(); });
    gallery.addEventListener('focusin', schedule);
    gallery.addEventListener('focusout', () => setTimeout(schedule, 0));
    document.addEventListener('visibilitychange', schedule);
    reducedMotion.addEventListener('change', (event) => { paused = event.matches; updatePlayback(); });
    gallery.addEventListener('keydown', (event) => {
        if (event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return;
        if (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight') return;
        event.preventDefault();
        const next = (current + (event.key === 'ArrowRight' ? 1 : -1) + slides.length) % slides.length;
        dots[next].focus();
        show(next);
    });

    gallery.setAttribute('aria-roledescription', 'carousel');
    show(0);
    updatePlayback();
    controls.hidden = false;
})();
