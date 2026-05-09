#!/usr/bin/env node
import express from 'express';
import type { Request, Response } from 'express';
import { dispatch, UnknownCommandError, type Handlers } from
    '../WebViewEmbed/web/src/generated/schema.handlers.ts';

const port = Number(process.argv[2] ?? 8088);

const handlers: Handlers = {
    ping: () => ({ pong: true, serverTimeMs: Date.now() }),
};

const app = express();
app.use(express.json());

app.post('/rpc', async (req: Request, res: Response) =>
{
    const { command, payload } = req.body ?? {};
    if (typeof command !== 'string')
        return res.status(400).json({ error: "Missing 'command' field" });

    try
    {
        const result = await dispatch(handlers, command, payload ?? {});
        res.json({ result: result ?? null });
    }
    catch (e)
    {
        if (e instanceof UnknownCommandError)
            return res.status(404).json({ error: e.message });
        res.status(500).json({ error: e instanceof Error ? e.message : String(e) });
    }
});

app.listen(port, () =>
{
    console.log(`RPC listening on http://localhost:${port}/rpc (Ctrl-C to quit)`);
});
