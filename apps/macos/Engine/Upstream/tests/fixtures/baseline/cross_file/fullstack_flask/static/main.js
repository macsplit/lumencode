function showItem(itemId) {
  return fetch(`/items/${itemId}`).then((r) => r.json());
}

function saveItem(item) {
  return fetch('/items', { method: 'POST', body: JSON.stringify(item) });
}
