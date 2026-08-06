// Generated. Do not edit by hand.
//
// React bindings over the eacp WebView bridge. Event names are constrained to
// `keyof E`, so typos and stale event references fail at compile time.

import { useEffect, useState, useSyncExternalStore } from 'react';
import { isBackendAvailable } from './backend';
import type { TransportOn } from './schema.bridge';

// `on` must reference the SAME TransportOn symbol the bridge module exports —
// a structurally identical local alias carries its own E and inference falls
// back to `object`, leaving K as `never` at the call site.
export interface EventCapableBackend<E extends object = object>
{
    on?: TransportOn<E>;
}

// Polls, because the host can't push a "ready" signal to code that loaded
// before the bridge was installed.
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

export interface NativeEventConfig<
    E extends object,
    K extends Extract<keyof E, string>,
>
{
    backend: EventCapableBackend<E>;
    event: K;
    initial: E[K];
}

//   export const useTick = makeNativeEvent({
//       backend, event: 'tick', initial: { angle: 0 },
//   });
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

//   export const useParameters = makeNativeState({
//       backend,
//       event: 'parameters',
//       setCommand: backend.setParameters,
//       initial: { level: 0.5, autoCycle: false, counter: 0 },
//   });
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

export interface BridgeStoreConfig<
    E extends object,
    K extends Extract<keyof E, string>,
>
{
    backend: EventCapableBackend<E>;
    event: K;
    fetch: () => Promise<E[K]>;
    initial: E[K];

    // Skips the initial fetch when it returns false. The hooks codegen wires
    // this to isBackendAvailable so `npm run dev` in a browser falls back to
    // `initial` instead of logging a rejection per hook.
    shouldFetch?: () => boolean;
}

// Bridges a C++-owned state value into useSyncExternalStore. Tear-free, and
// the initial fetch is built in. For keyed collections use makeKeyedStore.
//
//   export const useParameters = makeBridgeStore({
//       backend,
//       event: 'parameters',
//       fetch: backend.getParameters,
//       initial: { level: 0.5, autoCycle: false, counter: 0 },
//   });
//
// The fetch and the subscription start on first mount, NOT in this factory
// body — the body runs at module scope, before the importing page can call
// configureBridge, and a subscription made then binds to the unprefixed name
// for good. See WebViewSubApiReact's tests. Once started they stay for the
// life of the page, so a remount reads a current value.
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

    let started = false;

    const start = (): void =>
    {
        if (started)
            return;
        started = true;

        if (config.shouldFetch?.() ?? true)
        {
            void config.fetch().then(setSnapshot).catch(
                (err) => console.error('makeBridgeStore: initial fetch failed', err));
        }

        config.backend.on?.(config.event, setSnapshot);
    };

    const subscribe = (listener: () => void): (() => void) =>
    {
        start();
        listeners.add(listener);
        return () => { listeners.delete(listener); };
    };

    const getSnapshot = (): E[K] => snapshot;

    return function useStore(): E[K]
    {
        return useSyncExternalStore(subscribe, getSnapshot);
    };
}

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

    // Defaults to shallow equality over own enumerable properties.
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

// makeBridgeStore for a collection: item identity survives snapshots, so
// useItem(id) re-renders only when that id changes and useIds() only on
// add/remove/reorder. Starts lazily for the same reason as makeBridgeStore.
//
//   const todos = makeKeyedStore({
//       backend,
//       event: 'todos',
//       fetch: backend.getTodos,
//       initial: { items: [] },
//       getItems: s => s.items,
//       getKey:   i => i.id,
//   });
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

    let started = false;

    const start = (): void =>
    {
        if (started)
            return;
        started = true;

        if (config.shouldFetch?.() ?? true)
        {
            void config.fetch().then(apply).catch(
                (err) => console.error('makeKeyedStore: initial fetch failed', err));
        }

        config.backend.on?.(config.event, apply);
    };

    const subscribe = (listener: () => void): (() => void) =>
    {
        start();
        listeners.add(listener);
        return () => { listeners.delete(listener); };
    };

    return {
        useAll: () => useSyncExternalStore(subscribe, () => allSnapshot),
        useIds: () => useSyncExternalStore(subscribe, () => idsSnapshot),
        useItem: (id: Id) =>
            useSyncExternalStore(subscribe, () => itemsById.get(id)),
    };
}
