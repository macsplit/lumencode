const express = require('express');
const orders = require('./routes/orders');

const app = express();

app.get('/api/users/:id', (req, res) => {
  res.json({ id: req.params.id });
});

app.post('/login', function login(req, res) {
  res.redirect('/');
});

app.use('/api/orders', orders);

module.exports = app;
