import { makeBackend } from './schema.backend';
import { makeBridge, type Transport } from './schema.bridge';
import type { Events } from './schema.events';

interface EacpBridge
{
    invoke<Res = unknown, Req = unknown>(command: string, payload?: Req): Promise<Res>;
    on<T = unknown>(event: string, handler: (payload: T) => void): () => void;
    // Registers a function the native (C++) side can call via
    // WebViewBridge::call(name, ...) — the reverse of a command. May be
    // sync or async; its resolved value is sent back to C++.
    expose<Req = unknown, Res = unknown>(
        name: string,
        fn: (payload: Req) => Res | Promise<Res>,
    ): void;
}

declare global
{
    interface Window
    {
        // Optional — `window.eacp` is injected by the native WebView host.
        // Absent during `npm run dev` in a regular browser, which is a
        // supported workflow: hooks fall back to their initial values and
        // commands reject with a clear error.
        eacp?: EacpBridge;
    }
}

// Returns whether the native eacp WebView bridge is currently present.
// Evaluated lazily so workflows that inject the bridge after page load
// — or never inject it at all, e.g. `npm run dev` in Chrome — resolve
// to the right answer at call time rather than at module load.
export function isBackendAvailable(): boolean
{
    return typeof window !== 'undefined' && window.eacp != null;
}

// Registers a function the native (C++) side can call via
// WebViewBridge::call(name, ...). The reverse of a command: instead of
// the page invoking C++, C++ invokes the page, awaiting the result.
// No-op when the bridge isn't present (e.g. `npm run dev` in a browser),
// so app code can call it unconditionally at startup.
export function expose<Req = unknown, Res = unknown>(
    name: string,
    fn: (payload: Req) => Res | Promise<Res>,
): void
{
    if (! isBackendAvailable())
        return;
    window.eacp!.expose(name, fn);
}

// Command / event namespace prefix. Empty by default: a page that owns the
// whole bridge invokes 'group.getParams' directly.
//
// It is non-empty when this client is mounted as a SUB-API of a larger one. A
// host that reflects this API under a member name receives its commands as
// '<member>.group.getParams' and publishes its events as
// '<member>.group.values', so the host page calls configureBridge once before
// render and every generated command, event and hook routes through it.
//
// Read at call time rather than captured, so it also applies to the
// subscriptions the generated hooks set up after configureBridge runs.
let namespacePrefix = '';

export function configureBridge(options: { prefix?: string }): void
{
    namespacePrefix = options.prefix ?? '';
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
        return window.eacp!.on(namespacePrefix + event,
                               handler as (payload: unknown) => void);
    },
};

export const backend = makeBridge(webViewTransport, makeBackend);
