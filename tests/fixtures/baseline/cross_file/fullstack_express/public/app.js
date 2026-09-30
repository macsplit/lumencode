async function loadUser(id) {
  const response = await fetch('/api/users/' + id);
  return response.json();
}

function placeOrder(customer, order) {
  return axios.post(`/api/orders/${customer}/orders`, order);
}

function refreshUser(id) {
  $.ajax({ url: `/api/users/${id}`, type: 'GET' });
}
