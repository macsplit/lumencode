// The build script replaces this marker with a hash of the complete asset set.
const CACHE = 'lumencode-prototype-__ASSET_VERSION__';
const ASSETS = ['.', 'index.html', 'app.js', 'worker.js', 'style.css',
  'manifest.webmanifest', 'icon.svg', 'engine.js', 'engine.wasm'];
self.addEventListener('install', event => {
  event.waitUntil(caches.open(CACHE).then(cache => cache.addAll(ASSETS)));
});
self.addEventListener('activate', event => {
  event.waitUntil((async () => {
    for (const name of await caches.keys()) {
      if (name.startsWith('lumencode-prototype-') && name !== CACHE) await caches.delete(name);
    }
    await self.clients.claim();
  })());
});
self.addEventListener('fetch', event => {
  const url = new URL(event.request.url);
  if (event.request.method !== 'GET' || url.origin !== self.location.origin ||
      !url.pathname.startsWith(new URL(self.registration.scope).pathname)) return;
  event.respondWith(caches.open(CACHE).then(async cache => {
    const cached = await cache.match(event.request);
    if (cached) return cached;
    if (event.request.mode === 'navigate') return cache.match('index.html');
    return fetch(event.request);
  }));
});
