// The HTTP server the web's network tests talk to, since a wasm program cannot
// listen. Plain node, no packages.
//
//   node TestServer.mjs start <state.json>   start one in the background
//   node TestServer.mjs stop <state.json>    stop it
//   node TestServer.mjs serve [state.json]   run in the foreground
//
// It listens on two loopback ports, so a redirect can cross origins, and
// writes {"baseUrl", "otherBaseUrl", "pid"} to the state file once both are
// up. Every answer carries CORS headers, so a page on another origin can call
// it, unless the query has nocors=1.

import {spawn} from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import {fileURLToPath} from 'node:url';
import zlib from 'node:zlib';

const [mode = 'serve', statePath = ''] = process.argv.slice(2);
const idleLimitMs = 30 * 60 * 1000;

const exposedHeaders = ['X-Custom', 'Content-Range', 'Content-Encoding', 'Content-Length',
                        'Location', 'ETag', 'Accept-Ranges', 'X-Hits'].join(', ');

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

function pattern(offset, size)
{
    const bytes = Buffer.allocUnsafe(size);
    for (let i = 0; i < size; ++i)
        bytes[i] = (offset + i) % 251;
    return bytes;
}

function corsHeaders(req, url)
{
    if (url.searchParams.get('nocors') === '1')
        return {};

    return {'Access-Control-Allow-Origin': req.headers.origin || '*',
            'Access-Control-Expose-Headers': exposedHeaders,
            'Vary': 'Origin'};
}

async function readBody(req)
{
    const chunks = [];
    for await (const chunk of req)
        chunks.push(chunk);
    return Buffer.concat(chunks);
}

function createHandler(ports, hits)
{
    return async (req, res) =>
    {
        const url = new URL(req.url, 'http://127.0.0.1');
        const q = name => url.searchParams.get(name);
        const cors = corsHeaders(req, url);
        const send = (status, headers, body = '') =>
        {
            res.writeHead(status, {...cors, ...headers});
            res.end(body);
        };

        if (req.method === 'OPTIONS')
            return send(204, {'Access-Control-Allow-Methods': 'GET, POST, PUT, DELETE, PATCH, HEAD',
                              'Access-Control-Allow-Headers':
                                  req.headers['access-control-request-headers'] || '*',
                              'Access-Control-Max-Age': '600'});

        const body = await readBody(req);
        const port = req.socket.localPort;

        switch (url.pathname)
        {
            case '/get':
                return send(200, {'Content-Type': 'text/plain', 'X-Custom': 'custom-value'},
                            'hello from ' + port);

            case '/echo':
                return send(200, {'Content-Type': 'application/json'},
                            JSON.stringify({method: req.method, path: req.url, port,
                                            headers: req.headers, bodyLength: body.length,
                                            body: body.toString('latin1')}));

            case '/status':
            {
                const code = Number(q('code') || 404);
                return send(code, {'Content-Type': 'text/plain'}, 'status ' + code);
            }

            case '/slow-headers':
                await sleep(Number(q('ms') || 2000));
                return send(200, {'Content-Type': 'text/plain'}, 'finally');

            case '/slow-body':
                res.writeHead(200, {...cors, 'Content-Type': 'text/plain'});
                res.flushHeaders();
                await sleep(Number(q('ms') || 2000));
                return res.end('late body');

            case '/stall':
            {
                // `after` bytes, then nothing for `ms` (0: never), then the rest.
                const total = Number(q('total') || 100000);
                const after = Number(q('after') || 1000);
                const ms = Number(q('ms') || 0);
                res.writeHead(200, {...cors, 'Content-Type': 'application/octet-stream',
                                    'Content-Length': String(total)});
                res.write(pattern(0, after));
                if (ms === 0)
                    return;
                await sleep(ms);
                if (!res.destroyed)
                    res.end(pattern(after, total - after));
                return;
            }

            case '/drip':
            {
                const chunks = Number(q('chunks') || 10);
                const size = Number(q('size') || 1000);
                const ms = Number(q('ms') || 50);
                res.writeHead(200, {...cors, 'Content-Type': 'application/octet-stream',
                                    'Content-Length': String(chunks * size)});
                for (let i = 0; i < chunks && !res.destroyed; ++i)
                {
                    res.write(pattern(i * size, size));
                    await sleep(ms);
                }
                return res.end();
            }

            case '/bytes':
            {
                // `size` bytes of the pattern, without a Content-Length when
                // nolength=1.
                const size = Number(q('size') || 100000);
                const length = q('nolength') === '1' ? {} : {'Content-Length': String(size)};
                res.writeHead(200, {...cors, 'Content-Type': 'application/octet-stream', ...length});
                let sent = 0;
                while (sent < size && !res.destroyed)
                {
                    const n = Math.min(65536, size - sent);
                    if (!res.write(pattern(sent, n)))
                        await new Promise(resolve => res.once('drain', resolve));
                    sent += n;
                }
                return res.end();
            }

            case '/range':
            {
                const size = Number(q('size') || 100000);
                const base = {'Content-Type': 'application/octet-stream', 'Accept-Ranges': 'bytes'};
                const range = req.headers.range;
                if (!range)
                    return send(200, {...base, 'Content-Length': String(size)},
                                req.method === 'HEAD' ? '' : pattern(0, size));
                const match = /^bytes=(\d+)-(\d*)$/.exec(range);
                const start = match ? Number(match[1]) : size;
                if (start >= size)
                    return send(416, {...base, 'Content-Range': `bytes */${size}`});
                const end = match[2] ? Math.min(Number(match[2]), size - 1) : size - 1;
                return send(206, {...base, 'Content-Range': `bytes ${start}-${end}/${size}`,
                                  'Content-Length': String(end - start + 1)},
                            req.method === 'HEAD' ? '' : pattern(start, end - start + 1));
            }

            case '/redirect':
                return send(Number(q('code') || 302),
                            {'Location': q('to') || '/get', 'Content-Type': 'text/plain'},
                            'redirect body');

            case '/chain':
            {
                const n = Number(q('n') || 3);
                return send(302, {'Location': n <= 1 ? '/echo' : `/chain?n=${n - 1}`});
            }

            case '/cross':
            {
                const other = port === ports[0] ? ports[1] : ports[0];
                return send(Number(q('code') || 302),
                            {'Location': `http://127.0.0.1:${other}/echo`});
            }

            case '/gzip':
            {
                const encoded = zlib.gzipSync(Buffer.from('gzip body '.repeat(1000)));
                return send(200, {'Content-Type': 'text/plain', 'Content-Encoding': 'gzip',
                                  'Content-Length': String(encoded.length)}, encoded);
            }

            case '/cacheable':
            {
                // Counts its hits per key, and lets any cache keep it a minute.
                const key = q('key') || '';
                hits.set(key, (hits.get(key) || 0) + 1);
                return send(200, {'Content-Type': 'text/plain', 'Cache-Control': 'max-age=60',
                                  'X-Hits': String(hits.get(key))},
                            'hits ' + hits.get(key));
            }

            case '/file':
            {
                // A file of the test's own, such as a zip it just wrote.
                const file = q('path') || '';
                if (!fs.existsSync(file))
                    return send(404, {'Content-Type': 'text/plain'}, 'no such file');
                const bytes = fs.readFileSync(file);
                return send(200, {'Content-Type': 'application/octet-stream',
                                  'Content-Length': String(bytes.length)}, bytes);
            }

            case '/etag':
            {
                const tag = '"' + (q('tag') || 'v1') + '"';
                if (req.headers['if-none-match'] === tag)
                    return send(304, {'ETag': tag});
                return send(200, {'Content-Type': 'application/octet-stream', 'ETag': tag},
                            pattern(0, Number(q('size') || 5000)));
            }

            default:
                return send(404, {'Content-Type': 'text/plain'}, 'no such endpoint');
        }
    };
}

// A WebSocket endpoint, RFC 6455 by hand. /ws echoes every message, and picks
// the first subprotocol offered. A text message that is a command does
// something else instead:
//   "close <code> <reason>"  the server closes with that code
//   "send <n>"               the server sends n bytes of the pattern, binary
//   "ignore-close"           the client's close is not answered, and the
//                            connection dropped 2 s later
// The close the server sends back echoes the client's code and reason, so a
// test can see what was sent. /ws-reject refuses the handshake with a 403;
// /ws-hang does not answer it, and drops the connection 2 s later.
function webSocketFrame(opcode, payload)
{
    const length = payload.length;
    const header = length < 126 ? Buffer.from([0x80 | opcode, length])
                 : length < 65536 ? Buffer.from([0x80 | opcode, 126, length >> 8, length & 255])
                 : Buffer.concat([Buffer.from([0x80 | opcode, 127]),
                                  (() => { const b = Buffer.alloc(8);
                                           b.writeBigUInt64BE(BigInt(length)); return b; })()]);
    return Buffer.concat([header, payload]);
}

function closePayload(code, reason)
{
    const payload = Buffer.alloc(2 + Buffer.byteLength(reason));
    payload.writeUInt16BE(code, 0);
    payload.write(reason, 2);
    return payload;
}

function upgradeWebSocket(req, socket)
{
    const url = new URL(req.url, 'http://127.0.0.1');

    // Dropped after a while all the same: a client under node that never sees
    // its socket end keeps its process alive.
    if (url.pathname === '/ws-hang')
    {
        setTimeout(() => socket.destroy(), 2000);
        return;
    }

    if (url.pathname !== '/ws' || !req.headers['sec-websocket-key'])
    {
        socket.end('HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n');
        return;
    }

    const accept = crypto.createHash('sha1')
        .update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
        .digest('base64');
    const offered = (req.headers['sec-websocket-protocol'] || '')
        .split(',').map(p => p.trim()).filter(Boolean);
    const protocol = offered.length > 0 ? `Sec-WebSocket-Protocol: ${offered[0]}\r\n` : '';

    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n'
                 + `Connection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n${protocol}\r\n`);

    let buffered = Buffer.alloc(0);
    let fragments = [];
    let fragmentOpcode = 0;
    let ignoreClose = false;
    let closed = false;

    const send = (opcode, payload) => { if (!closed) socket.write(webSocketFrame(opcode, payload)); };

    const onMessage = (opcode, payload) =>
    {
        const text = opcode === 1 ? payload.toString('utf8') : '';
        const words = text.split(' ');

        if (words[0] === 'close' && words.length >= 2)
        {
            send(8, closePayload(Number(words[1]), words.slice(2).join(' ')));
            closed = true;
            socket.end();
        }
        else if (words[0] === 'send' && words.length === 2)
            send(2, pattern(0, Number(words[1])));
        else if (text === 'ignore-close')
            ignoreClose = true;
        else
            send(opcode, payload);
    };

    const onFrame = (fin, opcode, payload) =>
    {
        if (opcode === 8)
        {
            if (ignoreClose)
            {
                setTimeout(() => socket.destroy(), 2000);
                return;
            }
            send(8, payload);
            closed = true;
            socket.end();
        }
        else if (opcode === 9)
            send(10, payload);
        else if (opcode === 0 || opcode === 1 || opcode === 2)
        {
            if (opcode !== 0)
                fragmentOpcode = opcode;
            fragments.push(payload);
            if (fin)
            {
                onMessage(fragmentOpcode, Buffer.concat(fragments));
                fragments = [];
            }
        }
    };

    socket.on('data', chunk =>
    {
        buffered = Buffer.concat([buffered, chunk]);

        while (buffered.length >= 2)
        {
            const fin = (buffered[0] & 0x80) !== 0;
            const opcode = buffered[0] & 0x0f;
            const masked = (buffered[1] & 0x80) !== 0;
            let length = buffered[1] & 0x7f;
            let offset = 2;

            if (length === 126)
            {
                if (buffered.length < 4) return;
                length = buffered.readUInt16BE(2);
                offset = 4;
            }
            else if (length === 127)
            {
                if (buffered.length < 10) return;
                length = Number(buffered.readBigUInt64BE(2));
                offset = 10;
            }

            const maskOffset = offset;
            offset += masked ? 4 : 0;

            if (buffered.length < offset + length)
                return;

            const payload = Buffer.from(buffered.subarray(offset, offset + length));

            if (masked)
                for (let i = 0; i < payload.length; ++i)
                    payload[i] ^= buffered[maskOffset + (i % 4)];

            buffered = buffered.subarray(offset + length);
            onFrame(fin, opcode, payload);
        }
    });

    socket.on('error', () => {});
    socket.on('close', () => { closed = true; });
}

function listen(handler)
{
    return new Promise((resolve, reject) =>
    {
        const server = http.createServer((req, res) => handler(req, res).catch(error =>
        {
            try { res.writeHead(500); res.end(String(error)); } catch {}
        }));
        server.on('upgrade', upgradeWebSocket);
        server.keepAliveTimeout = 1000;
        server.on('error', reject);
        server.listen(0, '127.0.0.1', () => resolve(server));
    });
}

async function serve()
{
    const hits = new Map();
    const ports = [];
    const handler = createHandler(ports, hits);
    const servers = [await listen(handler), await listen(handler)];
    ports.push(...servers.map(server => server.address().port));

    const state = {baseUrl: `http://127.0.0.1:${ports[0]}`,
                   otherBaseUrl: `http://127.0.0.1:${ports[1]}`, pid: process.pid};
    console.log(JSON.stringify(state));

    if (statePath)
    {
        fs.writeFileSync(statePath + '.tmp', JSON.stringify(state));
        fs.renameSync(statePath + '.tmp', statePath);
    }

    setTimeout(() => process.exit(0), idleLimitMs).unref();
}

async function start()
{
    fs.rmSync(statePath, {force: true});
    const child = spawn(process.execPath, [fileURLToPath(import.meta.url), 'serve', statePath],
                        {detached: true, stdio: 'ignore'});
    child.unref();

    for (let waited = 0; waited < 10000; waited += 50)
    {
        if (fs.existsSync(statePath))
        {
            console.log(fs.readFileSync(statePath, 'utf8'));
            return;
        }
        await sleep(50);
    }

    console.error('the test server did not come up');
    process.exit(1);
}

function stop()
{
    try
    {
        const {pid} = JSON.parse(fs.readFileSync(statePath, 'utf8'));
        process.kill(pid);
    }
    catch {}

    fs.rmSync(statePath, {force: true});
}

if (mode === 'start')
    await start();
else if (mode === 'stop')
    stop();
else
    await serve();
