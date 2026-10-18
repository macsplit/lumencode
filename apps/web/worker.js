/* Classic worker: the engine, source reads and indexing stay off the UI thread. */
importScripts('engine.js');
const started = performance.now();
const enginePromise = createLumenCodeEngine({
  locateFile: name => new URL(name, self.location.href).href,
  printErr: message => postMessage({type: 'diagnostic', message}),
});
let mounted = false;
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
    if (operation === 'mount') {
      engine.request(JSON.stringify({operation: 'reset'}));
      if (mounted) engine.FS.unmount('/project');
      else engine.FS.mkdir('/project');
      const seen = new Set();
      const blobs = data.files.map(({path, file}) => {
        validatePath(path);
        if (seen.has(path)) throw new Error(`Duplicate project path: ${path}`);
        seen.add(path);
        return {name: path, data: file};
      });
      engine.FS.mount(engine.FS.filesystems.WORKERFS, {blobs}, '/project');
      for (const {path, file} of data.files) {
        engine.FS.utime(`/project/${path}`, file.lastModified, file.lastModified);
      }
      mounted = true;
      // Per-project cache is ephemeral in this first prototype.
      try { engine.FS.unlink('/cache/project-index.json'); } catch (_) { /* absent */ }
      result = JSON.parse(engine.request(JSON.stringify({operation: 'candidates', root: '/project'})));
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
    }
    if (result.error) throw new Error(result.error);
    postMessage({id, result, elapsedMs: performance.now() - begin,
      linearMemoryBytes: engine.HEAPU8?.byteLength ?? null});
  } catch (error) {
    postMessage({id, error: String(error), elapsedMs: performance.now() - begin});
  }
};
