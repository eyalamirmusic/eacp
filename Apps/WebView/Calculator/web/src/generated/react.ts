// Generated. Do not edit by hand.
//
// React-friendly bindings over the eacp WebView bridge. Two layers:
//
//   - Low-level hooks (useNativeEvent / useNativeState) take an
//     explicit backend, event name and command name on every call.
//   - High-level factories (makeNativeEvent / makeNativeState) close
//     over those values once at module scope and return a typed custom
//     hook so consumer components look like:
//         const [params, setParams] = useParameters();
//
// All helpers are generic over the bridge's event map E (typically the
// `Events` interface from the matching .events module). E is
// inferred from the `backend` argument and the event name is
// constrained to `keyof E`, so typos and stale event references fail
// at compile time. The constraint is `extends object` so plain
// interfaces satisfy it without declaring an index signature.

import { useEffect, useState, useSyncExternalStore } from 'react';
import { isBackendAvailable } from './backend';
import type { TransportOn } from './schema.bridge';

// `on` is typed as the same `TransportOn<E>` the bridge module exports
// — not a structurally equivalent local copy. TS only unifies the E
// parameter of `Bridge<X, Events>` against `EventCapableBackend<E>`
// when both sides reference the *same* TransportOn symbol; two
// identical-looking local aliases would each carry an independent E
// and inference would silently fall back to the `object` default
// (leaving K as `never` at the call site).
export interface EventCapableBackend<E extends object = object>
{
    on?: TransportOn<E>;
}

// ---------- backend availability ----------
//
// React-friendly view of isBackendAvailable() — useful when the
// component tree wants to show a "running without backend" banner or
// branch on the bridge presence in a way that survives a late-injected
// bridge (some dev workflows attach window.eacp asynchronously).
//
// Implemented with a low-frequency poll rather than a callback because
// the WebView host can't push a "ready" signal to JS code that loaded
// before the bridge was installed. 500 ms is a balance between dev
// responsiveness and idle CPU cost; it's fine to tune per app by
// wrapping `isBackendAvailable` in your own hook if the default isn't
// right.
export function useBackendAvailable(): boolean
{
    const [available, setAvailable] = useState<boolean>(isBackendAvailable);

    useEffect(() =>
    {
        const id = setInterval(() =>
        {
            const next = isBackendAvailable();
            setAvailable((prev) => prev === next ? prev : next);
        }, 500);

        return () => clearInterval(id);
    }, []);

    return available;
}

// ---------- low-level hooks ----------

export function useNativeEvent<
    E extends object,
    K extends Extract<keyof E, string>,
>(
    backend: EventCapableBackend<E>,
    eventName: K,
    initial: E[K],
): E[K]
{
    const [value, setValue] = useState<E[K]>(initial);

    useEffect(() => backend.on?.(eventName, setValue), [backend, eventName]);

    return value;
}

export function useNativeState<
    E extends object,
    K extends Extract<keyof E, string>,
>(
    backend: EventCapableBackend<E>,
    eventName: K,
    setCommand: (req: E[K]) => Promise<void>,
    initial: E[K],
): [E[K], (next: E[K]) => void]
{
    const [value, setValue] = useState<E[K]>(initial);

    useEffect(() => backend.on?.(eventName, setValue), [backend, eventName]);

    const set = (next: E[K]): void =>
    {
        setValue(next);
        void setCommand(next).catch(
            (err) => console.error('useNativeState: setCommand failed', err));
    };

    return [value, set];
}

// ---------- module-scope factories ----------

export interface NativeEventConfig<
    E extends object,
    K extends Extract<keyof E, string>,
>
{
    backend: EventCapableBackend<E>;
    event: K;
    initial: E[K];
}

// Builds a custom hook bound to one event source. Call at module
// scope; export the result as a `use*` named hook.
//
//   export const useTick = makeNativeEvent({
//       backend, event: 'tick', initial: { angle: 0 },
//   });
//
//   function Component() { const tick = useTick(); ... }
export function makeNativeEvent<
    E extends object,
    K extends Extract<keyof E, string>,
>(config: NativeEventConfig<E, K>): () => E[K]
{
    const { backend, event, initial } = config;

    return function useEvent(): E[K]
    {
        return useNativeEvent(backend, event, initial);
    };
}

export interface NativeStateConfig<
    E extends object,
    K extends Extract<keyof E, string>,
>
{
    backend: EventCapableBackend<E>;
    event: K;
    setCommand: (req: E[K]) => Promise<void>;
    initial: E[K];
}

// Builds a custom hook bound to one bidirectional state binding.
// The setter is a typed command reference (e.g. backend.setParameters),
// not a string, so typos are caught at compile time. Call at module
// scope; export the result as a `use*` named hook.
//
//   export const useParameters = makeNativeState({
//       backend,
//       event: 'parameters',
//       setCommand: backend.setParameters,
//       initial: { level: 0.5, autoCycle: false, counter: 0 },
//   });
//
//   function Component()
//   {
//       const [params, setParams] = useParameters();
//       ...
//   }
export function makeNativeState<
    E extends object,
    K extends Extract<keyof E, string>,
>(
    config: NativeStateConfig<E, K>,
): () => [E[K], (next: E[K]) => void]
{
    const { backend, event, setCommand, initial } = config;

    return function useState(): [E[K], (next: E[K]) => void]
    {
        return useNativeState(backend, event, setCommand, initial);
    };
}

// ---------- External-store factory ----------
//
// Bridges a C++-owned state value into a React `useSyncExternalStore`
// hook. Compared with makeNativeState:
//
//   - Concurrent-mode safe: getSnapshot is read on every render, so
//     React can never tear against the live store.
//   - Initial fetch is built in: `fetch` is invoked once at module load
//     so the first render has real data instead of waiting for the next
//     C++ broadcast.
//   - No setter is baked in. Action-style commands (add/toggle/remove)
//     don't fit the "one set" shape; call typed commands on `backend`
//     directly from event handlers.
//
// For keyed collections — i.e. a state whose payload contains a
// vector of items, each identified by some id field — use
// `makeKeyedStore` (defined further down) instead. It gives you
// `{useAll, useIds, useItem}` hooks with per-item identity preservation
// so a `<Row id={42}>` only re-renders when item 42 actually changes.
//
//   export const useParameters = makeBridgeStore({
//       backend,
//       event: 'parameters',
//       fetch: backend.getParameters,
//       initial: { level: 0.5, autoCycle: false, counter: 0 },
//   });
//
//   function Component() { const params = useParameters(); ... }
export interface BridgeStoreConfig<
    E extends object,
    K extends Extract<keyof E, string>,
>
{
    backend: EventCapableBackend<E>;
    event: K;
    fetch: () => Promise<E[K]>;
    initial: E[K];

    // Optional guard: when supplied, the initial fetch is skipped if
    // this returns false. The hooks codegen wires this to
    // `isBackendAvailable` so `npm run dev` in a regular browser falls
    // back silently to `initial` instead of logging a missing-bridge
    // rejection per hook. Omit (or return true) to always fetch.
    shouldFetch?: () => boolean;
}

export function makeBridgeStore<
    E extends object,
    K extends Extract<keyof E, string>,
>(config: BridgeStoreConfig<E, K>): () => E[K]
{
    let snapshot: E[K] = config.initial;
    const listeners = new Set<() => void>();

    const setSnapshot = (next: E[K]): void =>
    {
        snapshot = next;
        for (const listener of listeners) listener();
    };

    const subscribe = (listener: () => void): (() => void) =>
    {
        listeners.add(listener);
        return () => { listeners.delete(listener); };
    };

    const getSnapshot = (): E[K] => snapshot;

    if (config.shouldFetch?.() ?? true)
    {
        void config.fetch().then(setSnapshot).catch(
            (err) => console.error('makeBridgeStore: initial fetch failed', err));
    }

    config.backend.on?.(config.event, setSnapshot);

    return function useStore(): E[K]
    {
        return useSyncExternalStore(subscribe, getSnapshot);
    };
}

// ---------- Keyed-collection store ----------
//
// Same wire-format contract as makeBridgeStore (fetch + event), but
// the payload is treated as a keyed collection: items are indexed by
// the result of `getKey(item)`, identity is preserved across snapshots
// for items whose fields haven't changed (per `eq`), and the ids list
// is only swapped when the order/membership actually shifts.
//
// The returned `useItem(id)` hook re-renders only when that specific
// id's record changes; `useIds()` re-renders only on add/remove/
// reorder; `useAll()` re-renders on every store update.
//
// Default equality is shallow over enumerable own properties — fine
// for the typical "flat record" payload (id + a few primitives). Pass
// `eq` if items contain nested objects you want to compare deeply, or
// if reference equality is the only thing you care about (`eq: Object.is`).
//
//   const todos = makeKeyedStore({
//       backend,
//       event: 'todos',
//       fetch: backend.getTodos,
//       initial: { items: [] },
//       getItems: s => s.items,
//       getKey:   i => i.id,
//   });
//
//   export const useTodos     = todos.useAll;
//   export const useTodoIds   = todos.useIds;
//   export const useTodoItem  = todos.useItem;

export interface KeyedStoreConfig<
    E extends object,
    K extends Extract<keyof E, string>,
    Item,
    Id,
>
{
    backend: EventCapableBackend<E>;
    event: K;
    fetch: () => Promise<E[K]>;
    initial: E[K];
    getItems: (state: E[K]) => readonly Item[];
    getKey: (item: Item) => Id;
    eq?: (a: Item, b: Item) => boolean;

    // See BridgeStoreConfig.shouldFetch.
    shouldFetch?: () => boolean;
}

export interface KeyedStore<
    E extends object,
    K extends Extract<keyof E, string>,
    Item,
    Id,
>
{
    useAll: () => E[K];
    useIds: () => readonly Id[];
    useItem: (id: Id) => Item | undefined;
}

function shallowEqual<T extends object>(a: T, b: T): boolean
{
    if (a === b) return true;
    const aKeys = Object.keys(a) as (keyof T)[];
    const bKeys = Object.keys(b) as (keyof T)[];
    if (aKeys.length !== bKeys.length) return false;
    for (const key of aKeys)
        if (a[key] !== b[key]) return false;
    return true;
}

export function makeKeyedStore<
    E extends object,
    K extends Extract<keyof E, string>,
    Item extends object,
    Id,
>(config: KeyedStoreConfig<E, K, Item, Id>): KeyedStore<E, K, Item, Id>
{
    const eq = config.eq ?? shallowEqual;

    let allSnapshot: E[K] = config.initial;
    let idsSnapshot: readonly Id[] = config.getItems(config.initial)
        .map(config.getKey);
    let itemsById = new Map<Id, Item>();
    for (const item of config.getItems(config.initial))
        itemsById.set(config.getKey(item), item);

    const listeners = new Set<() => void>();

    const apply = (next: E[K]): void =>
    {
        allSnapshot = next;

        const nextItems = config.getItems(next);

        const nextItemsById = new Map<Id, Item>();
        for (const item of nextItems)
        {
            const id = config.getKey(item);
            const prev = itemsById.get(id);
            nextItemsById.set(id, prev !== undefined && eq(prev, item) ? prev : item);
        }
        itemsById = nextItemsById;

        const nextIds = nextItems.map(config.getKey);
        const idsEqual = nextIds.length === idsSnapshot.length
            && nextIds.every((id, i) => id === idsSnapshot[i]);
        if (!idsEqual) idsSnapshot = nextIds;

        for (const listener of listeners) listener();
    };

    const subscribe = (listener: () => void): (() => void) =>
    {
        listeners.add(listener);
        return () => { listeners.delete(listener); };
    };

    if (config.shouldFetch?.() ?? true)
    {
        void config.fetch().then(apply).catch(
            (err) => console.error('makeKeyedStore: initial fetch failed', err));
    }

    config.backend.on?.(config.event, apply);

    return {
        useAll: () => useSyncExternalStore(subscribe, () => allSnapshot),
        useIds: () => useSyncExternalStore(subscribe, () => idsSnapshot),
        useItem: (id: Id) =>
            useSyncExternalStore(subscribe, () => itemsById.get(id)),
    };
}
