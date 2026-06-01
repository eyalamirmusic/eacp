import { createRoot } from 'react-dom/client';
import App from './App';
import { expose } from './generated/backend';
import './style.css';

const container = document.getElementById('app');
if (!container) throw new Error('#app container not found');

createRoot(container).render(<App />);

// Demonstrates the native -> page call path: the C++ side can ask the
// live UI for something only it knows — here, the todo texts exactly as
// rendered in the DOM. Async on purpose (awaits a tick) to show that
// WebViewBridge::call awaits a Promise-returning page function, not just
// a synchronous one. Driven from WebViewTodoTests' call<> test.
expose<void, { count: number; texts: string[] }>('getRenderedTodos', async () =>
{
    await new Promise<void>((resolve) => setTimeout(resolve, 0));

    const nodes = Array.from(
        document.querySelectorAll('[data-testid="todo-text"]'));
    const texts = nodes.map((node) => node.textContent ?? '');

    return { count: texts.length, texts };
});
