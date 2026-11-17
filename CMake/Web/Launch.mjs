// Runs one built web app. Plain node, no packages.
//
//   node Launch.mjs <node|browser|headless> <app.js> [options] [-- <app arguments>...]
//   node Launch.mjs open <file>
//
//   node       runs <app.js> under this node
//   browser    serves its directory as plain static files on a loopback port
//              and opens its own page, <app>.html, in the default browser
//   headless   the same in headless Chrome, for CI and checks, in a bare page
//   open       opens <file> (an app's page, the gallery's index.html) in the
//              default browser as a file:// URL and exits: no server
//
// In the browser and headless modes the page relays the app's output and exit
// code back, so this prints the same lines node would and exits with the
// app's code; an app that never exits is served until this is interrupted.
//
//   --companion <script.mjs>   start `node <script.mjs> serve` first, wait for
//                              the JSON line with a baseUrl it prints, append
//                              that URL to the app's arguments, stop it after
//   --chrome <path>            Chrome for headless, which fails after 120 s
//   --timeout <seconds>        that limit, for a suite that runs longer
//   --webgpu                   give headless Chrome its GPU, so the page has
//                              WebGPU (it runs with --disable-gpu otherwise)

import {spawn} from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import readline from 'node:readline';
import {pathToFileURL} from 'node:url';

const timeoutExitCode = 124;
const headlessTimeoutSeconds = 120;

// A page outlives its program, so whatever calls into the exited runtime soon
// after is still relayed, and an abort then still fails the run.
const afterExitMs = 1000;
let appExitCode = null;
const cleanups = [];
let finishing = false;

function fail(message, code = 1)
{
    console.error(`launch: ${message}`);
    return finish(code);
}

async function finish(code)
{
    if (finishing)
        return;

    finishing = true;

    for (const cleanup of cleanups.reverse())
    {
        try { await cleanup(); }
        catch (error) { console.error(`launch: cleanup failed: ${error.message}`); }
    }

    for (const stream of [process.stdout, process.stderr])
        await new Promise(resolve => stream.write('', resolve));

    process.exit(code);
}

function parseCommandLine(argv)
{
    const separator = argv.indexOf('--');
    const own = separator < 0 ? argv : argv.slice(0, separator);
    const appArguments = separator < 0 ? [] : argv.slice(separator + 1);
    const [mode, app, ...rest] = own;
    const options = {mode, app, appArguments, companion: '', chrome: '',
                     webgpu: false, timeout: headlessTimeoutSeconds};

    for (let i = 0; i < rest.length; i += 2)
    {
        if (rest[i] === '--webgpu')
        {
            options.webgpu = true;
            --i;
            continue;
        }

        const value = rest[i + 1];

        if (value === undefined)
            throw new Error(`${rest[i]} needs a value`);

        if (rest[i] === '--companion')
            options.companion = path.resolve(value);
        else if (rest[i] === '--chrome')
            options.chrome = value;
        else if (rest[i] === '--timeout')
            options.timeout = Number(value);
        else
            throw new Error(`unknown option ${rest[i]}`);
    }

    if (!['node', 'browser', 'headless', 'open'].includes(mode) || !app)
        throw new Error('usage: Launch.mjs <node|browser|headless> <app.js> '
                        + '[--companion <script>] [--chrome <path>] '
                        + '[-- <app arguments>...], or Launch.mjs open <file>');

    options.app = path.resolve(app);

    if (!fs.existsSync(options.app))
        throw new Error(`no app at ${options.app}`);

    return options;
}

function waitForExit(child, ms)
{
    if (child.exitCode !== null || child.signalCode !== null)
        return Promise.resolve(true);

    return new Promise(resolve =>
    {
        const timer = setTimeout(() => resolve(false), ms);
        child.once('exit', () =>
        {
            clearTimeout(timer);
            resolve(true);
        });
    });
}

function nodeArguments(script, args)
{
    return [...process.execArgv, script, ...args];
}

async function startCompanion(script)
{
    const child = spawn(process.execPath, nodeArguments(script, ['serve']),
                        {stdio: ['ignore', 'pipe', 'inherit']});

    cleanups.push(async () =>
    {
        child.kill('SIGTERM');

        if (!await waitForExit(child, 2000))
            child.kill('SIGKILL');
    });

    const lines = readline.createInterface({input: child.stdout});

    const baseUrl = await new Promise((resolve, reject) =>
    {
        const timer = setTimeout(() => reject(new Error('did not come up in 10 s')), 10000);
        child.once('exit', code => reject(new Error(`exited with ${code} before it listened`)));
        lines.on('line', line =>
        {
            try
            {
                const state = JSON.parse(line);

                if (state.baseUrl)
                {
                    clearTimeout(timer);
                    resolve(state.baseUrl);
                }
            }
            catch {}
        });
    });

    lines.removeAllListeners('line');
    lines.on('line', line => console.error(line));
    return baseUrl;
}

async function runUnderNode(options, args)
{
    const child = spawn(process.execPath, nodeArguments(options.app, args),
                        {stdio: 'inherit'});

    cleanups.push(async () =>
    {
        child.kill('SIGTERM');

        if (!await waitForExit(child, 2000))
            child.kill('SIGKILL');
    });

    child.on('error', error => fail(`cannot run node: ${error.message}`));
    child.on('exit', (code, signal) =>
        finish(code ?? 128 + (os.constants.signals[signal] ?? 0)));
}

function pageFor(appFile, args)
{
    const config = JSON.stringify({script: appFile, arguments: args})
                       .replaceAll('<', '\\u003c');

    return `<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>${appFile.replace(/\.js$/, '')}</title>
<style>
  body { margin: 0; font: 13px/1.4 ui-monospace, Menlo, Consolas, monospace;
         background: #fff; color: #111; }
  pre { margin: 0; padding: 16px; white-space: pre-wrap; }
  .err { color: #b00; }
  .status { color: #666; }
  @media (prefers-color-scheme: dark)
  {
    body { background: #111; color: #ddd; }
    .err { color: #f77; }
  }
</style>
</head>
<body>
<pre id="output"></pre>
<script>
(() =>
{
    const config = ${config};
    const output = document.getElementById('output');
    const queue = [];
    let sending = false;

    const pump = () =>
    {
        if (sending || queue.length === 0)
            return;

        sending = true;
        const batch = queue.splice(0);

        fetch('/events', {method: 'POST', body: JSON.stringify(batch)})
            .catch(() => {})
            .finally(() => { sending = false; pump(); });
    };

    const report = event =>
    {
        queue.push(event);
        pump();
    };

    const show = (text, kind) =>
    {
        const line = document.createElement('span');
        line.className = kind;
        line.textContent = text + '\\n';
        output.appendChild(line);
    };

    const print = (stream, text) =>
    {
        show(text, stream === 'err' ? 'err' : '');
        report({kind: stream, text});
    };

    const exited = code =>
    {
        show('exited with code ' + code, 'status');
        report({kind: 'exit', code});
    };

    window.addEventListener('error', e => print('err', 'page error: ' + e.message));
    window.addEventListener('unhandledrejection',
                            e => print('err', 'unhandled rejection: ' + e.reason));

    globalThis.Module = {
        arguments: config.arguments,
        print: text => print('out', text),
        printErr: text => print('err', text),
        onExit: exited,
        onAbort: reason =>
        {
            print('err', 'aborted: ' + reason);
            exited(1);
        },
    };

    const script = document.createElement('script');
    script.src = config.script;
    script.onerror = () =>
    {
        print('err', 'could not load ' + config.script);
        exited(1);
    };
    document.body.appendChild(script);
})();
</script>
</body>
</html>
`;
}

// UTF-8 named, since a -sSINGLE_FILE .js carries its wasm as UTF-8 text.
const contentTypes = {'.js': 'text/javascript; charset=utf-8',
                      '.mjs': 'text/javascript; charset=utf-8',
                      '.wasm': 'application/wasm', '.json': 'application/json',
                      '.html': 'text/html; charset=utf-8', '.map': 'application/json',
                      '.css': 'text/css', '.svg': 'image/svg+xml', '.png': 'image/png'};

function appName(appFile)
{
    return path.basename(appFile).replace(/\.js$/, '');
}

// The app's own page, relaying (relay=1), with these arguments in place of
// the ones written into it; force=1 when a companion is running, which the
// page otherwise says a static host lacks.
function appPageUrl(options, args)
{
    const params = new URLSearchParams({relay: '1'});

    if (options.companion)
        params.set('force', '1');

    for (const arg of args)
        params.append('arg', arg);

    return `/${encodeURIComponent(appName(options.app))}.html?${params}`;
}

// What each mode serves from the app's directory: headless the bare page at /,
// which the ctest entries were written against; browser the directory, with /
// sent to the app's page.
function siteFor(options, args)
{
    const root = path.dirname(options.app);

    if (options.mode === 'headless')
        return {root, redirect: '', pages: {'/': ['text/html; charset=utf-8',
                                                  pageFor(path.basename(options.app), args)]}};

    return {root, redirect: appPageUrl(options, args), pages: {}};
}

function fileIn(root, pathname)
{
    const file = path.resolve(root, '.' + pathname);

    if (file.startsWith(root + path.sep) && fs.statSync(file, {throwIfNoEntry: false})
                                                ?.isFile())
        return file;

    return '';
}

const noCache = {'Cache-Control': 'no-store'};

async function readBody(req)
{
    const chunks = [];

    for await (const chunk of req)
        chunks.push(chunk);

    return Buffer.concat(chunks).toString('utf8');
}

function serveApp(options, args, onEvent)
{
    const site = siteFor(options, args);

    const server = http.createServer(async (req, res) =>
    {
        const url = new URL(req.url, 'http://127.0.0.1');

        if (req.method === 'POST' && url.pathname === '/events')
        {
            const body = await readBody(req);
            res.writeHead(204, noCache);
            res.end();

            try { JSON.parse(body).forEach(onEvent); }
            catch {}

            return;
        }

        const page = site.pages[url.pathname];

        if (page)
        {
            res.writeHead(200, {...noCache, 'Content-Type': page[0]});
            return res.end(page[1]);
        }

        if (url.pathname === '/' && site.redirect)
        {
            res.writeHead(302, {...noCache, Location: site.redirect});
            return res.end();
        }

        const file = fileIn(site.root, decodeURIComponent(url.pathname));

        if (!file)
        {
            res.writeHead(404, noCache);
            return res.end();
        }

        fs.readFile(file, (error, data) =>
        {
            if (error)
            {
                res.writeHead(404, noCache);
                return res.end();
            }

            const type = contentTypes[path.extname(file)] ?? 'application/octet-stream';
            res.writeHead(200, {...noCache, 'Content-Type': type});
            res.end(data);
        });
    });

    cleanups.push(() =>
    {
        server.closeAllConnections();
        return new Promise(resolve => server.close(() => resolve()));
    });

    return new Promise((resolve, reject) =>
    {
        server.on('error', reject);
        server.listen(0, '127.0.0.1',
                      () => resolve(`http://127.0.0.1:${server.address().port}/`));
    });
}

function relayEvent(event)
{
    if (event.kind === 'out')
        console.log(event.text);
    else if (event.kind === 'err')
        console.error(event.text);
    else if (event.kind === 'exit')
        exitAfterAWhile(Number(event.code) || 0);
}

function exitAfterAWhile(code)
{
    if (appExitCode === null)
        setTimeout(() => finish(appExitCode), afterExitMs);

    appExitCode = appExitCode || code;
}

// Resolves to whether the opener started.
function openInDefaultBrowser(url)
{
    const [command, args] = {darwin: ['open', [url]],
                             win32: ['cmd', ['/c', 'start', '""', url]]}[process.platform]
                            ?? ['xdg-open', [url]];

    const child = spawn(command, args, {stdio: 'ignore', detached: true,
                                        windowsVerbatimArguments: true});
    child.unref();

    return new Promise(resolve =>
    {
        child.once('spawn', () => resolve(true));
        child.once('error', error =>
        {
            console.error(`launch: cannot open ${url}: ${error.message}`);
            resolve(false);
        });
    });
}

async function openFile(file)
{
    const url = pathToFileURL(file).href;
    console.error(`launch: opening ${url}`);
    return finish(await openInDefaultBrowser(url) ? 0 : 1);
}

function stopChrome(chrome)
{
    if (process.platform === 'win32')
        return spawn('taskkill', ['/pid', String(chrome.pid), '/T', '/F'], {stdio: 'ignore'});

    try { process.kill(-chrome.pid, 'SIGTERM'); }
    catch {}
}

function killChromeNow(chrome)
{
    if (process.platform === 'win32' || chrome.exitCode !== null)
        return;

    try { process.kill(-chrome.pid, 'SIGKILL'); }
    catch {}
}

// Headless Chrome exposes WebGPU only with its GPU process: with --webgpu the
// GPU stays on, and --enable-unsafe-webgpu lets it through where the platform
// has not shipped it yet (Linux).
function headlessGpuFlags(webgpu)
{
    return webgpu ? ['--enable-unsafe-webgpu'] : ['--disable-gpu'];
}

function startHeadlessChrome(executable, url, webgpu)
{
    const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'eacp-launch-chrome-'));
    const chrome = spawn(executable,
                         ['--headless=new', '--no-first-run', '--no-default-browser-check',
                          ...headlessGpuFlags(webgpu), `--user-data-dir=${profile}`, url],
                         {stdio: ['ignore', 'pipe', 'pipe'],
                          detached: process.platform !== 'win32'});
    let log = '';
    const keep = chunk => { log = (log + chunk).slice(-4000); };
    chrome.stdout.on('data', keep);
    chrome.stderr.on('data', keep);

    process.on('exit', () => killChromeNow(chrome));
    cleanups.push(async () =>
    {
        stopChrome(chrome);

        if (!await waitForExit(chrome, 3000))
            killChromeNow(chrome);

        await waitForExit(chrome, 2000);
        fs.rmSync(profile, {recursive: true, force: true, maxRetries: 5});
    });

    chrome.on('error', error => fail(`cannot start Chrome: ${error.message}`));
    chrome.on('exit', code =>
    {
        if (!finishing)
            fail(`Chrome exited (${code}) before the app did\n${log}`);
    });
}

async function runInBrowser(options, args)
{
    const page = options.app.replace(/\.js$/, '.html');

    if (options.mode === 'browser' && !fs.existsSync(page))
        return fail(`no ${path.basename(page)} beside the app; eacp_web_executable `
                    + 'writes it when CMake configures');

    const url = await serveApp(options, args, relayEvent);

    if (options.mode === 'browser')
    {
        console.error(`launch: serving ${path.basename(options.app)} at ${url}`);
        openInDefaultBrowser(url);
        return;
    }

    if (!fs.existsSync(options.chrome))
        return fail('no Chrome; configure with -DEACP_WEB_CHROME=<path>, or pass '
                    + '--chrome <path>');

    setTimeout(() => fail(`the app did not exit within ${options.timeout} s`,
                          timeoutExitCode),
               options.timeout * 1000).unref();
    startHeadlessChrome(options.chrome, url, options.webgpu);
}

async function main()
{
    for (const [signal, number] of [['SIGINT', 2], ['SIGTERM', 15]])
        process.on(signal, () => finish(128 + number));

    let options;

    try { options = parseCommandLine(process.argv.slice(2)); }
    catch (error) { return fail(error.message, 2); }

    if (options.mode === 'open')
        return openFile(options.app);

    const args = [...options.appArguments];

    if (options.companion)
    {
        try { args.push(await startCompanion(options.companion)); }
        catch (error) { return fail(`companion ${options.companion}: ${error.message}`); }
    }

    if (options.mode === 'node')
        return runUnderNode(options, args);

    try { await runInBrowser(options, args); }
    catch (error) { return fail(error.message); }
}

main();
