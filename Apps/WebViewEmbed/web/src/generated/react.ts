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
// `ServerEvents` interface from the matching .events module). E is
// inferred from the `backend` argument and the event name is
// constrained to `keyof E`, so typos and stale event references fail
// at compile time. The constraint is `extends object` so plain
// interfaces satisfy it without declaring an index signature.

import { useEffect, useState, useSyncExternalStore } from 'react';
import type { TransportOn } from './schema.bridge';

// `on` is typed as the same `TransportOn<E>` the bridge module exports
// — not a structurally equivalent local copy. TS only unifies the E
// parameter of `Bridge<X, ServerEvents>` against `EventCapableBackend<E>`
// when both sides reference the *same* TransportOn symbol; two
// identical-looking local aliases would each carry an independent E
// and inference would silently fall back to the `object` default
// (leaving K as `never` at the call site).
export interface EventCapableBackend<E extends object = object>
{
    on?: TransportOn<E>;
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
// Selector hooks (re-render only on the slice you read) can be layered
// on top by hand-writing a store with a Map-by-id + identity-preserving
// apply step and exposing per-slice `useSyncExternalStore` hooks.
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

    void config.fetch().then(setSnapshot).catch(
        (err) => console.error('makeBridgeStore: initial fetch failed', err));

    config.backend.on?.(config.event, setSnapshot);

    return function useStore(): E[K]
    {
        return useSyncExternalStore(subscribe, getSnapshot);
    };
}
