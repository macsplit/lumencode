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
    this.indexing = false;
    this.viewer = null;
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
    if (this.indexing && ['analyse', 'read'].includes(data.operation)) {
      this.view(data).then(response => this.emit({...response, id:data.id}))
        .catch(error => this.emit({id:data.id,error:String(error)}));
      return;
    }
    if (data.operation !== 'index') { this.coordinator.postMessage(data); return; }
    this.indexing = true;
    this.index(data).then(result => {
      const inspectionMemory = this.viewer?.memory || 0;
      this.indexing = false;
      this.emit({...result,id:data.id,inspectionLinearMemoryCapacityBytes:inspectionMemory,
        aggregateLinearMemoryCapacityBytes:(result.aggregateLinearMemoryCapacityBytes || 0) + inspectionMemory});
    })
      .catch(error => { this.indexing = false; this.emit({id:data.id,error:String(error)}); })
      .finally(() => { this.indexing = false; this.releaseViewer(); });
  }
  async view(data) {
    if (!this.viewer) {
      const worker = new Worker('worker.js');
      const viewer = this.viewer = {worker, pending:new Map(), sequence:0, users:0};
      worker.onmessage = ({data: response}) => {
        viewer.memory = Math.max(viewer.memory || 0,response.linearMemoryBytes || 0);
        if (response.type === 'fatal') {
          for (const {reject} of viewer.pending.values()) reject(new Error(response.error));
          viewer.pending.clear();
        }
        const pending = viewer.pending.get(response.id);
        if (pending) {
          viewer.pending.delete(response.id);
          response.error ? pending.reject(new Error(response.error)) : pending.resolve(response);
        }
      };
      worker.onerror = event => {
        for (const {reject} of viewer.pending.values()) reject(new Error(event.message));
        viewer.pending.clear();
      };
      viewer.request = (operation, extra = {}) => new Promise((resolve, reject) => {
        const id = ++viewer.sequence;
        viewer.pending.set(id, {resolve,reject});
        worker.postMessage({...extra,id,operation});
      });
      viewer.ready = viewer.request('mount', {files:this.files,parserOnly:true});
    }
    const viewer = this.viewer;
    viewer.users++;
    try {
      await viewer.ready;
      if (this.closed) throw new Error('Project closed');
      // This worker has no graph. The UI refreshes the current view with indexed
      // relationships once the coordinator finishes, preserving view position.
      return await viewer.request(data.operation, {...data,indexed:false});
    } finally {
      viewer.users--; this.releaseViewer();
    }
  }
  releaseViewer(force = false) {
    const viewer = this.viewer;
    if (!viewer || (!force && (this.indexing || viewer.users))) return;
    viewer.worker.terminate();
    for (const {reject} of viewer.pending.values()) reject(new Error('Project closed'));
    viewer.pending.clear(); this.viewer = null;
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
    this.releaseViewer(true);
    this.coordinator.terminate();
    for (const {reject} of this.pending.values()) reject(new Error('Indexing cancelled'));
    this.pending.clear();
    this.files = [];
  }
}
globalThis.LumenCodeWorker = LumenCodeWorker;
