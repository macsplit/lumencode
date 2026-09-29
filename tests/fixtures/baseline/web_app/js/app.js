const list = document.getElementById('todo-list');
const input = document.querySelector('#new-todo');

function handleKey(event) {
    if (event.key === 'Enter') {
        addTodo(input.value);
    }
}

function addTodo(title) {
    const item = document.createElement('li');
    item.textContent = title;
    item.classList.add('completed');
    list.appendChild(item);
}

function clearCompleted() {
    list.querySelectorAll('li.completed').forEach((item) => item.remove());
    document.getElementById('footer').classList.toggle('hidden');
}

class TodoCounter extends HTMLElement {
    connectedCallback() {
        this.textContent = String(list.children.length);
    }
}

customElements.define('todo-counter', TodoCounter);
