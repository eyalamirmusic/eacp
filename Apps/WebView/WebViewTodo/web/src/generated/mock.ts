// Generated. Do not edit by hand.
//
// Dev/test mock of the native bridge: installs a window.eacp that loops
// invoke()/on()/expose() back in-process, so the same generated client runs
// unchanged in a plain browser (`npm run dev`) or a jsdom test. The scenario —
// typed command handlers plus an event timeline — is supplied by the app.
import { dispatch, type Handlers } from './schema.handlers';
import type { EventName, Events } from './schema.events';

export interface MockControl
{
    // Push a typed event to on()/hook subscribers (e.g. progress, channel).
    emit<K extends EventName>(name: K, payload: Events[K]): void;
    // Drive a function the page registered via expose() — the native -> page
    // call direction — for tests that exercise C++-initiated calls.
    call<Res = unknown>(name: string, payload?: unknown): Promise<Res>;
}

export interface MockScenario
{
    // The same contract C++ implements. A command you did not supply rejects
    // with a clear error rather than silently resolving undefined.
    handlers?: Partial<Handlers>;
    // Drives events over time; re-run whenever the scenario is (re)installed.
    run?: (control: MockControl) => void | Promise<void>;
}

type Listener = (payload: unknown) => void;

function guarded(partial: Partial<Handlers>): Handlers
{
    return new Proxy(partial, {
        get: (target, key) =>
            (target as Record<string | symbol, unknown>)[key] ??
            (() =>
            {
                throw new Error(`eacp mock: no handler for command '${String(key)}'`);
            }),
    }) as Handlers;
}

export function installMock(scenario: MockScenario = {}): MockControl
{
    const listeners = new Map<string, Set<Listener>>();
    const exposed = new Map<string, (payload: unknown) => unknown>();
    const handlers = guarded(scenario.handlers ?? {});

    const control: MockControl = {
        emit: (name, payload) =>
        {
            for (const handler of [...(listeners.get(name) ?? [])])
                handler(payload);
        },
        call: (name, payload) =>
        {
            const fn = exposed.get(name);
            if (! fn)
                return Promise.reject(
                    new Error(`eacp mock: no exposed function '${name}'`));
            return Promise.resolve(fn(payload)) as Promise<never>;
        },
    };

    window.eacp = {
        invoke: (command: string, payload?: unknown) =>
            dispatch(handlers, command, payload),
        on: (event: string, handler: Listener) =>
        {
            const set = listeners.get(event) ?? new Set<Listener>();
            listeners.set(event, set);
            set.add(handler);
            return () =>
            {
                set.delete(handler);
            };
        },
        expose: (name: string, fn: (payload: unknown) => unknown) =>
        {
            exposed.set(name, fn);
        },
    } as unknown as Window['eacp'];

    void scenario.run?.(control);
    return control;
}
