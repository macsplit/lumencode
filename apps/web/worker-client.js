/* All workers belong to this client, so cancel/restart terminates the entire pool.
 * Each parser runs the same single-threaded WASM engine against the same files.
 * Only the coordinator owns the index and its persistent cache. */
class LumenCodeWorker extends EventTarget {
  constructor() {
    super();
    this.addEventListener('message', event => this.onmessage?.(event));
    this.addEventListener('error', event => this.onerror?.(event));
    this.coordinator = new Worker('worker.js');
    this.children = new Set();
    this.pending = new Map();
    this.sequence = 0;
    this.closed = false;
    this.files = [];
    this.coordinator.onmessage = event => {
      const {data} = event;
      const pending = this.pending.get(data.id);
      if (pending) {
        this.pending.delete(data.id);
        data.error ? pending.reject(new Error(data.error)) : pending.resolve(data);
      } else this.emit(data);
    };
    this.coordinator.onerror = event => {
      this.dispatchEvent(new ErrorEvent('error', {message: event.message}));
    };
  }
  emit(data) {
    if (this.closed) return;
    const event = new MessageEvent('message', {data});
    this.dispatchEvent(event);
  }
  request(operation, extra = {}) {
    return new Promise((resolve, reject) => {
      const id = `internal-${++this.sequence}`;
      this.pending.set(id, {resolve, reject});
      this.coordinator.postMessage({id, operation, ...extra});
    });
  }
  postMessage(data) {
    if (this.closed) return;
    if (data.operation === 'mount') this.files = data.files;
    if (data.operation !== 'index') { this.coordinator.postMessage(data); return; }
    this.index(data).then(result => this.emit({...result, id: data.id}))
      .catch(error => this.emit({id: data.id, error: String(error)}));
  }
  async index(data) {
    const begin = performance.now();
    const {result: plan} = await this.request('index-plan');
    // Small projects avoid extra module startup. An explicit count is useful for
    // reproducible comparisons; the UI uses at most four parsers, leaving a core.
    const requested = data.workers ?? Math.min(4, Math.max(1, (navigator.hardwareConcurrency || 2) - 1));
    const count = Math.min(4, Math.max(1, Math.floor(requested)), plan.paths.length);
    if (count < 2 || (data.workers == null && plan.paths.length < 128)) {
      const result = await this.request('index');
      return {...result, elapsedMs: performance.now() - begin, parserWorkers: 1,
        aggregateLinearMemoryCapacityBytes: result.linearMemoryBytes};
    }
    const sizes = new Map(this.files.map(({path, file}) => [`/project/${path}`, file.size]));
    // Largest files start first to reduce the long tail without changing facts.
    const queue = [...plan.paths].sort((a, b) => (sizes.get(b) || 0) - (sizes.get(a) || 0));
    const facts = Object.create(null);
    let done = 0;
    let parserMemory = 0;
    try {
      await Promise.all(Array.from({length: count}, async () => {
        const child = new Worker('worker.js');
        const state = {worker: child, reject: null};
        this.children.add(state);
        let serial = 0;
        const call = (operation, extra = {}) => new Promise((resolve, reject) => {
          state.reject = reject;
          child.onmessage = ({data: response}) => {
            if (response.type === 'fatal') reject(new Error(response.error));
            if (response.id === serial) {
              state.reject = null;
              response.error ? reject(new Error(response.error)) : resolve(response);
            }
          };
          child.onerror = event => reject(new Error(event.message));
          child.postMessage({id: ++serial, operation, ...extra});
        });
        await call('mount', {files: this.files, parserOnly: true});
        let memory = 0;
        while (queue.length && !this.closed) {
          const path = queue.shift();
          this.emit({type: 'progress', phase: 'analysing', path, done, total: plan.paths.length});
          const response = await call('facts', {path: path.slice('/project/'.length)});
          facts[path] = response.result;
          memory = Math.max(memory, response.linearMemoryBytes || 0);
          this.emit({type: 'progress', phase: 'analysing', path, done: ++done, total: plan.paths.length});
        }
        parserMemory += memory;
      }));
    } finally {
      this.stopChildren();
    }
    if (this.closed) throw new Error('Indexing cancelled');
    if (done !== plan.paths.length) throw new Error('Incomplete parallel extraction');
    const result = await this.request('index', {facts});
    return {...result, elapsedMs: performance.now() - begin, parserWorkers: count,
      aggregateLinearMemoryCapacityBytes: parserMemory + (result.linearMemoryBytes || 0)};
  }
  stopChildren() {
    for (const state of this.children) {
      state.worker.terminate();
      state.reject?.(new Error('Indexing cancelled'));
    }
    this.children.clear();
  }
  terminate() {
    this.closed = true;
    this.stopChildren();
    this.coordinator.terminate();
    for (const {reject} of this.pending.values()) reject(new Error('Indexing cancelled'));
    this.pending.clear();
    this.files = [];
  }
}
globalThis.LumenCodeWorker = LumenCodeWorker;
