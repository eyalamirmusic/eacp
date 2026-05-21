import { useSyncExternalStore } from 'react';
import { backend } from './generated/backend';
import type { TodoItem, TodoState } from './generated/schema';

export interface TodoSummary
{
    total: number;
    completed: number;
}

const emptyIds: readonly number[] = [];
const emptySummary: TodoSummary = { total: 0, completed: 0 };

class TodoStore
{
    private ids: readonly number[] = emptyIds;
    private itemsById = new Map<number, TodoItem>();
    private summary: TodoSummary = emptySummary;
    private listeners = new Set<() => void>();

    constructor()
    {
        void backend.getTodos().then((initial) => this.apply(initial));
        backend.on?.('todos', (payload) => this.apply(payload));
    }

    subscribe = (listener: () => void): (() => void) =>
    {
        this.listeners.add(listener);
        return () => { this.listeners.delete(listener); };
    };

    getIds = (): readonly number[] => this.ids;

    getItem = (id: number): TodoItem | undefined => this.itemsById.get(id);

    getSummary = (): TodoSummary => this.summary;

    private apply(next: TodoState): void
    {
        const nextItems = new Map<number, TodoItem>();
        for (const item of next.items)
        {
            const prev = this.itemsById.get(item.id);
            const unchanged = prev !== undefined
                && prev.text === item.text
                && prev.completed === item.completed;
            nextItems.set(item.id, unchanged ? prev : item);
        }
        this.itemsById = nextItems;

        const nextIds = next.items.map((i) => i.id);
        const idsEqual = nextIds.length === this.ids.length
            && nextIds.every((id, i) => id === this.ids[i]);
        if (!idsEqual) this.ids = nextIds;

        const completed = next.items.reduce(
            (n, i) => (i.completed ? n + 1 : n), 0);
        if (completed !== this.summary.completed
            || next.items.length !== this.summary.total)
            this.summary = { total: next.items.length, completed };

        for (const listener of this.listeners) listener();
    }
}

const store = new TodoStore();

export function useTodoIds(): readonly number[]
{
    return useSyncExternalStore(store.subscribe, store.getIds);
}

export function useTodoItem(id: number): TodoItem | undefined
{
    return useSyncExternalStore(
        store.subscribe,
        () => store.getItem(id));
}

export function useTodoSummary(): TodoSummary
{
    return useSyncExternalStore(store.subscribe, store.getSummary);
}

export const addTodo        = (text: string)             => backend.addTodo({ text });
export const toggleTodo     = (id: number)               => backend.toggleTodo({ id });
export const editTodo       = (id: number, text: string) => backend.editTodo({ id, text });
export const removeTodo     = (id: number)               => backend.removeTodo({ id });
export const clearCompleted = ()                         => backend.clearCompleted();
