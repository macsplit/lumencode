const express = require('express');
const router = express.Router();

function createOrder(req, res) {
  res.status(201).json(req.body);
}

router.post('/:customer/orders', createOrder);

module.exports = router;
