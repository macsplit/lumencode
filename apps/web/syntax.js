/* Bounded, offline syntax colouring. The shared engine still owns analysis. */
globalThis.Syntax = (() => {
  const aliases = {script:'javascript', js:'javascript', ts:'typescript', html:'markup', xml:'markup',
    objc:'objectivec', 'objective-c':'objectivec', 'c++':'cpp', 'c#':'csharp', cs:'csharp',
    shell:'bash', sh:'bash', vbnet:'visual-basic', vb:'visual-basic', 'visual basic':'visual-basic',
    'visual basic .net':'visual-basic', py:'python', md:'markdown'};
  const extensions = {js:'javascript',mjs:'javascript',cjs:'javascript',jsx:'jsx',ts:'typescript',tsx:'tsx',
    html:'markup',htm:'markup',xml:'markup',svg:'markup',css:'css',scss:'scss',less:'less',
    php:'php',py:'python',c:'c',h:'c',cc:'cpp',cpp:'cpp',cxx:'cpp',hpp:'cpp',cs:'csharp',
    java:'java',swift:'swift',m:'objectivec',mm:'objectivec',rs:'rust',go:'go',rb:'ruby',
    kt:'kotlin',kts:'kotlin',sh:'bash',bash:'bash',sql:'sql',vb:'visual-basic',qml:'qml',
    json:'json',yaml:'yaml',yml:'yaml',toml:'toml',ini:'ini',md:'markdown',markdown:'markdown',diff:'diff',patch:'diff'};
  const supported = new Set(Object.values(extensions));
  let worker, active, queue = [], nextId = 0;
  const cache = new Map(); // At most two bounded sources/excerpts; promises also deduplicate refreshes.
  function language(label, path = '') {
    const extension = path.split('.').at(-1).toLowerCase();
    if (['jsx','tsx','scss','less','mm'].includes(extension)) return extensions[extension];
    const value = String(label || '').toLowerCase();
    return aliases[value] || (supported.has(value) ? value : extensions[extension]) || '';
  }
  function stop() {
    worker?.terminate(); worker = null;
    if (active) { clearTimeout(active.timer); active.resolve(null); active = null; }
    for (const task of queue) task.resolve(null);
    queue = [];
  }
  function pump() {
    if (active || !queue.length) return;
    active = queue.shift();
    try {
      if (!worker) {
        worker = new Worker('syntax-worker.js');
        worker.onmessage = ({data}) => {
          if (!active || data.id !== active.id) return;
          clearTimeout(active.timer); active.resolve(data.result); active = null; pump();
        };
        worker.onerror = event => { event.preventDefault(); stop(); };
      }
      active.timer = setTimeout(stop, 1500);
      worker.postMessage({id:active.id, text:active.text, language:active.language});
    } catch (_) { stop(); }
  }
  function highlight(text, label, path) {
    const grammar = language(label, path);
    if (!grammar || !text || text.length > 524288) return Promise.resolve(null);
    const key = grammar + '\0' + text;
    if (cache.has(key)) return cache.get(key);
    // Keep a rapid navigation sequence from accumulating background work.
    if (queue.length >= 4) queue.shift().resolve(null);
    const promise = new Promise(resolve => { queue.push({id:++nextId,text,language:grammar,resolve}); pump(); });
    cache.set(key, promise);
    while (cache.size > 2) cache.delete(cache.keys().next().value);
    return promise;
  }
  function fragment(parts) {
    const result = document.createDocumentFragment();
    for (const [text, type] of parts) {
      if (!type) result.append(document.createTextNode(text));
      else {
        const span = document.createElement('span'); span.className = 'syntax-' + type;
        span.textContent = text; result.append(span);
      }
    }
    return result;
  }
  function paint(element, text, label, path) {
    highlight(text, label, path).then(result => {
      if (!result || !element.isConnected || element.textContent !== text) return;
      if (result.lines.reduce((sum,line)=>sum+line.length,0) > 8000) return;
      const content = document.createDocumentFragment();
      result.lines.forEach((line,index) => { content.append(fragment(line)); if (result.endings[index]) content.append(result.endings[index]); });
      element.replaceChildren(content);
    });
  }
  function reset() { stop(); cache.clear(); }
  return {language, highlight, fragment, paint, reset};
})();
