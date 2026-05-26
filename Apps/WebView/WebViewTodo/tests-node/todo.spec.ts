import { test, expect, launchApp, type LaunchedApp } from './eacp-test-node.ts';

const SELECTORS = {
    input: '[data-testid="todo-input"]',
    add: '[data-testid="todo-add"]',
    item: '[data-testid="todo-item"]',
    text: '[data-testid="todo-text"]',
    toggle: '[data-testid="todo-toggle"]',
    remove: '[data-testid="todo-remove"]',
    remaining: '[data-testid="todo-remaining"]',
    completed: '[data-testid="todo-completed"]',
    clearCompleted: '[data-testid="todo-clear-completed"]',
} as const;

function lastItem(child: string): string
{
    return `${SELECTORS.item}:last-child ${child}`;
}

let app: LaunchedApp;

test.beforeEach(async () =>
{
    app = await launchApp();
    await app.driver.waitFor(SELECTORS.input);
});

test.afterEach(async () =>
{
    await app?.close();
});

test('seeds three todos on startup', async () =>
{
    expect(await app.driver.count(SELECTORS.item)).toBe(3);
});

test('adds a new todo via the form', async () =>
{
    const before = await app.driver.count(SELECTORS.item);

    await app.driver.fill(SELECTORS.input, 'Buy milk');
    await app.driver.click(SELECTORS.add);

    expect(await app.driver.count(SELECTORS.item)).toBe(before + 1);
    expect(await app.driver.text(lastItem(SELECTORS.text))).toBe('Buy milk');
});

test('toggle flips completion and updates the footer counts', async () =>
{
    const before = Number.parseInt(await app.driver.text(SELECTORS.remaining), 10);

    await app.driver.click(`${SELECTORS.item}:first-child ${SELECTORS.toggle}`);

    const remaining = Number.parseInt(await app.driver.text(SELECTORS.remaining), 10);
    expect(remaining).toBe(before - 1);
});

test('removing a todo decrements the count', async () =>
{
    const before = await app.driver.count(SELECTORS.item);

    await app.driver.click(`${SELECTORS.item}:first-child ${SELECTORS.remove}`);

    expect(await app.driver.count(SELECTORS.item)).toBe(before - 1);
});

test('domain RPCs are still reachable through the same bridge', async () =>
{
    // The bridge is shared with WebViewBridge, so the production
    // commands the React app calls (addTodo / getTodos) are also
    // reachable from the harness — handy for setting up state without
    // going through the UI.
    type TodoState = { items: Array<{ id: number; text: string; completed: boolean }> };

    const beforeState = await app.driver.invoke<TodoState>('getTodos');
    await app.driver.invoke('addTodo', { text: 'Direct add via bridge' });
    const afterState = await app.driver.invoke<TodoState>('getTodos');

    expect(afterState.items.length).toBe(beforeState.items.length + 1);
    expect(afterState.items.at(-1)?.text).toBe('Direct add via bridge');
});
