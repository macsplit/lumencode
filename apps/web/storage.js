/* Browser-local metadata and derived facts only; never uploads project files. */
(() => {
  let database;
  function open() {
    if (!database) database = new Promise((resolve, reject) => {
      const request = indexedDB.open('lumencode-local-projects', 1);
      request.onupgradeneeded = () => {
        request.result.createObjectStore('projects', {keyPath: 'id'});
        request.result.createObjectStore('indexes', {keyPath: 'key'});
      };
      request.onsuccess = () => {
        const db = request.result;
        db.onversionchange = () => { db.close(); database = undefined; };
        resolve(db);
      };
      request.onerror = () => reject(request.error);
      request.onblocked = () => reject(new Error('Local storage is blocked'));
    });
    return database;
  }
  async function transaction(store, mode, action) {
    const db = await open();
    return new Promise((resolve, reject) => {
      const tx = db.transaction(store, mode);
      let result;
      const request = action(tx.objectStore(store));
      if (request) request.onsuccess = () => { result = request.result; };
      tx.oncomplete = () => resolve(result);
      tx.onerror = () => reject(tx.error || new Error('Local storage failed'));
      tx.onabort = () => reject(tx.error || new Error('Local storage was interrupted'));
    });
  }
  const get = (store, key) => transaction(store, 'readonly', table => table.get(key));
  const all = store => transaction(store, 'readonly', table => table.getAll());
  const put = (store, value) => transaction(store, 'readwrite', table => table.put(value));
  globalThis.LumenCodeStorage = {
    projects: () => all('projects'),
    async saveProject(value) {
      await put('projects', value);
      const entries = (await all('projects')).sort((a, b) => b.opened - a.opened);
      for (const entry of entries.slice(8))
        await transaction('projects', 'readwrite', table => table.delete(entry.id));
    },
    index: key => get('indexes', key),
    async saveIndex(key, text) {
      await put('indexes', {key, text, updated: Date.now()});
      // Bound retained derived indexes; source files remain outside this store.
      const entries = (await all('indexes')).sort((a, b) => b.updated - a.updated);
      for (const entry of entries.slice(3))
        await transaction('indexes', 'readwrite', table => table.delete(entry.key));
    },
    async clear() {
      await transaction('indexes', 'readwrite', table => table.clear());
      await transaction('projects', 'readwrite', table => table.clear());
    },
  };
})();
