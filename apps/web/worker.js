/* Classic worker: the engine, source reads and indexing stay off the UI thread. */
importScripts('engine-version.js', 'storage.js', 'engine.js');
const started = performance.now();
const enginePromise = createLumenCodeEngine({
  locateFile: name => new URL(name, self.location.href).href,
  printErr: message => postMessage({type: 'diagnostic', message}),
});
let mounted = false;
let mountedFilesystem;
let persistentCacheKey;

async function cacheKey(projectKey, files, verifyContents) {
  if (!projectKey) return null;
  // Conservative whole-project invalidation also covers changed neighbour files
  // and package metadata used by the canonical parser's dependency resolution.
  const manifest = files.map(({path, file}) => [path, file.size, file.lastModified]);
  if (verifyContents) {
    // Imports lack a stable directory handle. Verify bytes so two unrelated
    // folders with the same name/size/timestamps cannot share stale facts.
    let done = 0;
    for (let i = 0; i < files.length; i++) {
      const {path, file} = files[i];
      const chunks = [];
      for (let offset = 0; offset < file.size; offset += 4 * 1024 * 1024) {
        const digest = await crypto.subtle.digest('SHA-256',
          await file.slice(offset, offset + 4 * 1024 * 1024).arrayBuffer());
        chunks.push([...new Uint8Array(digest)]);
      }
      manifest[i].push(chunks);
      postMessage({type: 'progress', operation: 'mount', phase: 'cache-check', path, done: ++done, total: files.length});
    }
  }
  manifest.sort((a, b) => a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0);
  const bytes = new TextEncoder().encode(JSON.stringify(manifest));
  const digest = await crypto.subtle.digest('SHA-256', bytes);
  const hex = [...new Uint8Array(digest)].map(byte => byte.toString(16).padStart(2, '0')).join('');
  return `facts-v1:${LUMENCODE_ENGINE_VERSION}:${projectKey}:${hex}`;
}
enginePromise.then(engine => {
  engine.FS.mkdir('/cache');
  postMessage({type: 'ready', initializationMs: performance.now() - started});
}).catch(error => postMessage({type: 'fatal', error: String(error)}));

function validatePath(path) {
  if (!path || path.startsWith('/') || path.includes('\\') ||
      path.split('/').some(part => !part || part === '.' || part === '..')) {
    throw new Error('Invalid project-relative path');
  }
}

self.onmessage = async ({data}) => {
  const {id, operation} = data;
  const begin = performance.now();
  try {
    const engine = await enginePromise;
    let result;
    let cacheStatus = 'session';
    if (operation === 'mount') {
      engine.request(JSON.stringify({operation: 'reset'}));
      if (mounted && mountedFilesystem === 'workerfs') engine.FS.unmount('/project');
      else if (mounted) throw new Error('Restart the worker before replacing a memory-backed profiling project');
      else engine.FS.mkdir('/project');
      const seen = new Set();
      const blobs = data.files.map(({path, file}) => {
        validatePath(path);
        if (seen.has(path)) throw new Error(`Duplicate project path: ${path}`);
        seen.add(path);
        return {name: path, data: file};
      });
      mountedFilesystem = data.filesystem || 'workerfs';
      if (mountedFilesystem === 'memory') {
        // Diagnostic comparison only: pre-read once to separate filesystem costs
        // from parsing. The UI continues to use file-backed WORKERFS by default.
        const reader = new FileReaderSync();
        for (const {path, file} of data.files) {
          const absolute = `/project/${path}`;
          engine.FS.mkdirTree(absolute.slice(0, absolute.lastIndexOf('/')));
          engine.FS.writeFile(absolute, new Uint8Array(reader.readAsArrayBuffer(file)));
        }
      } else if (mountedFilesystem === 'workerfs') {
        engine.FS.mount(engine.FS.filesystems.WORKERFS, {blobs}, '/project');
      } else throw new Error('Unknown profiling filesystem');
      for (const {path, file} of data.files) {
        engine.FS.utime(`/project/${path}`, file.lastModified, file.lastModified);
      }
      mounted = true;
      try { engine.FS.unlink('/cache/project-index.json'); } catch (_) { /* absent */ }
      persistentCacheKey = await cacheKey(data.projectKey, data.files, data.verifyContents);
      let cacheRestored = false;
      if (persistentCacheKey) {
        try {
          const saved = await LumenCodeStorage.index(persistentCacheKey);
          if (saved && typeof saved.text === 'string') {
            engine.FS.writeFile('/cache/project-index.json', saved.text);
            cacheRestored = true;
          }
          cacheStatus = cacheRestored ? 'restored' : 'cold';
        } catch (_) { cacheStatus = 'unavailable'; }
      }
      result = data.parserOnly ? {} : JSON.parse(engine.request(JSON.stringify({operation: 'candidates', root: '/project'})));
      result.cacheRestored = cacheRestored;
    } else if (operation === 'forget-cache') {
      persistentCacheKey = null;
      result = {forgotten: true};
    } else if (operation === 'read') {
      validatePath(data.path);
      result = {text: engine.FS.readFile(`/project/${data.path}`, {encoding: 'utf8'})};
    } else {
      if (!mounted) throw new Error('Choose a project first');
      const input = {...data, root: '/project'};
      if (data.path) {
        validatePath(data.path);
        input.path = `/project/${data.path}`;
      }
      result = JSON.parse(engine.request(JSON.stringify(input)));
      if (operation === 'index' && persistentCacheKey && !result.error) {
        try {
          await LumenCodeStorage.saveIndex(persistentCacheKey,
            engine.FS.readFile('/cache/project-index.json', {encoding: 'utf8'}));
          cacheStatus = 'saved';
        } catch (_) { cacheStatus = 'unavailable'; }
      }
    }
    if (result.error) throw new Error(result.error);
    postMessage({id, result, cacheStatus, elapsedMs: performance.now() - begin,
      linearMemoryBytes: engine.HEAPU8?.byteLength ?? null});
  } catch (error) {
    postMessage({id, error: String(error), elapsedMs: performance.now() - begin});
  }
};
