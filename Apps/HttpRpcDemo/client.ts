import { makeBackend } from '../WebViewEmbed/web/src/generated/schema.backend.ts';

const baseUrl = process.argv[2] ?? 'http://localhost:8088/rpc';

const invoke = async (command: string, payload: unknown): Promise<unknown> =>
{
    const res = await fetch(baseUrl, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ command, payload }),
    });
    const body = await res.json() as { result?: unknown; error?: string };
    if (!res.ok)
        throw new Error(body.error ?? `HTTP ${res.status}`);
    return body.result;
};

const client = makeBackend(invoke);

const reply = await client.ping();
console.log(`ping -> pong=${reply.pong} serverTimeMs=${reply.serverTimeMs}`);
