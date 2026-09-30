from flask import Flask, jsonify

app = Flask(__name__)


@app.route("/items/<int:item_id>")
def get_item(item_id):
    return jsonify({"id": item_id})


@app.route("/items", methods=["POST"])
def create_item():
    return jsonify({}), 201
