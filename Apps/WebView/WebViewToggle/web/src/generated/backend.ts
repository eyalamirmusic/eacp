import { makeBackend } from './schema.backend';
import { makeBridge, type Transport } from './schema.bridge';
import type { Events } from './schema.events';

interface EacpBridge
{
    invoke<Res = unknown, Req = unknown>(command: string, payload?: Req): Promise<Res>;
    on<T = unknown>(event: string, handler: (payload: T) => void): () => void;
    // Registers a function C++ can call via WebViewBridge::call(name, ...) —
    // the reverse of a command. Sync or async; the resolved value is sent back.
    expose<Req = unknown, Res = unknown>(
        name: string,
        fn: (payload: Req) => Res | Promise<Res>,
    ): void;
}

declare global
{
    interface Window
    {
        // Injected by the native WebView host. Absent during `npm run dev` in
        // a browser, which is supported: hooks fall back to their initial
        // values and commands reject with a clear error.
        eacp?: EacpBridge;
    }
}

export function isBackendAvailable(): boolean
{
    return typeof window !== 'undefined' && window.eacp != null;
}

// No-op when the bridge isn't present, so app code can call it unconditionally.
export function expose<Req = unknown, Res = unknown>(
    name: string,
    fn: (payload: Req) => Res | Promise<Res>,
): void
{
    if (! isBackendAvailable())
        return;
    window.eacp!.expose(name, fn);
}

// Command / event namespace prefix. Empty by default; non-empty when this
// client is mounted as a SUB-API, where the host receives its commands as
// '<member>.group.getParams' and publishes events as '<member>.group.values'.
let namespacePrefix = '';

// Live subscriptions, tracked so configureBridge can re-point them. Unlike
// invoke, which resolves the prefix per call, window.eacp.on commits to one
// name at subscribe time — so anything that subscribed first would otherwise
// stay bound to the unprefixed name, silently receiving nothing.
interface Subscription
{
    event: string;
    handler: (payload: unknown) => void;
    unsubscribe: () => void;
}

const subscriptions = new Set<Subscription>();

function bind(subscription: Subscription): void
{
    subscription.unsubscribe = window.eacp!.on(
        namespacePrefix + subscription.event, subscription.handler);
}

// Safe to call at any point in startup. Commands issued beforehand still
// resolve against the old prefix — they are one-shot calls with no later
// correction — so a page that invokes at module scope should configure first.
export function configureBridge(options: { prefix?: string }): void
{
    const next = options.prefix ?? '';
    if (next === namespacePrefix)
        return;

    namespacePrefix = next;

    for (const subscription of subscriptions)
    {
        subscription.unsubscribe();
        bind(subscription);
    }
}

const webViewTransport: Transport<Events> = {
    invoke: async (command, payload) =>
    {
        const name = namespacePrefix + command;
        if (! isBackendAvailable())
        {
            return Promise.reject(new Error(
                `eacp backend unavailable (cannot invoke '${name}')`));
        }
        return window.eacp!.invoke(name, payload);
    },
    on: (event, handler) =>
    {
        if (! isBackendAvailable())
            return () => {};

        const subscription: Subscription = {
            event,
            handler: handler as (payload: unknown) => void,
            unsubscribe: () => {},
        };

        bind(subscription);
        subscriptions.add(subscription);

        return () =>
        {
            subscription.unsubscribe();
            subscriptions.delete(subscription);
        };
    },
};

export const backend = makeBridge(webViewTransport, makeBackend);
