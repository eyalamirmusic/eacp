// Generated. Do not edit by hand.
//
// Pre-wired React hooks for every registered bridge event.
// Initial values come from toJSON(T{}).

import { backend, isBackendAvailable } from './backend';
import { makeKeyedStore } from './react';

const todosStore = makeKeyedStore({
    backend,
    event: 'todos',
    fetch: backend.getTodos,
    shouldFetch: isBackendAvailable,
    initial: {"items":[]},
    getItems: (s) => s.items,
    getKey:   (i) => i.id,
});
export const useTodos = todosStore.useAll;
export const useTodoIds = todosStore.useIds;
export const useTodoItem = todosStore.useItem;
