import { spawn, type ChildProcess } from 'node:child_process';
import { once } from 'node:events';
import { setTimeout as delay } from 'node:timers/promises';

import { AppDriver, type AppDriverOptions } from './AppDriver.ts';

export interface LaunchOptions extends AppDriverOptions
{
    /**
     * Path to the test-host executable, e.g.
     *   build/Apps/WebView/WebViewTodo/WebViewTodoTestHost.app/Contents/MacOS/WebViewTodoTestHost
     * The launcher does not look inside .app bundles; pass the inner
     * Mach-O directly. If omitted, falls back to the
     * EACP_TEST_HOST_BINARY env var (set by the CMake test entry).
     */
    bundle?: string;

    /** Extra args to pass to the child process. --rpc-port=0 is always added. */
    args?: readonly string[];

    /** Env vars to pass through. process.env is inherited by default. */
    env?: Readonly<Record<string, string>>;

    /** Cwd for the child process. Defaults to dirname(bundle). */
    cwd?: string;

    /**
     * Max time to wait for the EACP_RPC_PORT=<n> line on stdout. The
     * macOS first-launch path (entitlements + signature checks) can
     * take a beat — 10s is plenty in steady state.
     */
    startupTimeoutMs?: number;

    /**
     * Forwarded to the test harness as --rpc-port=<n>. Default 0 lets
     * the OS pick an ephemeral port; the launcher reads the bound
     * port back from the child's stdout.
     */
    rpcPort?: number;

    /** Pipe child stdout/stderr to this writer (e.g. process.stderr). */
    logger?: { write(chunk: string): void };
}

export interface LaunchedApp
{
    readonly driver: AppDriver;
    readonly process: ChildProcess;
    readonly rpcPort: number;
    close(): Promise<void>;
}

const portLineRegex = /EACP_RPC_PORT=(\d+)/;

export async function launchApp(options: LaunchOptions = {}): Promise<LaunchedApp>
{
    const bundle = options.bundle ?? process.env['EACP_TEST_HOST_BINARY'];
    if (!bundle)
    {
        throw new Error(
            'launchApp(): no bundle path. Pass `bundle:` or set '
            + 'EACP_TEST_HOST_BINARY. Run via `ctest -R <YourApp>Tests` after '
            + '`cmake --build build --target <YourApp>TestHost` to set it '
            + 'automatically.');
    }

    const args = [`--rpc-port=${options.rpcPort ?? 0}`, ...(options.args ?? [])];

    const child = spawn(bundle, args, {
        cwd: options.cwd,
        env: options.env !== undefined ? { ...process.env, ...options.env } : process.env,
        stdio: ['ignore', 'pipe', 'pipe'],
    });

    const teardown = async (): Promise<void> =>
    {
        if (child.exitCode !== null || child.signalCode !== null) return;
        child.kill('SIGTERM');
        // Cleanup race: if the child ignores SIGTERM the test would
        // hang. Give it a beat, then escalate.
        const exited = once(child, 'exit').then(() => true);
        const timeout = delay(2000).then(() => false);
        const ok = await Promise.race([exited, timeout]);
        if (!ok) child.kill('SIGKILL');
    };

    try
    {
        const rpcPort = await waitForPort(child, options.startupTimeoutMs ?? 10_000,
                                          options.logger);

        const driver = new AppDriver(`http://127.0.0.1:${rpcPort}/rpc`, {
            defaultTimeoutMs: options.defaultTimeoutMs,
        });

        // Health probe — the port line means listen() returned, but the
        // dispatcher thread pool might not have a worker on hand yet
        // for the very first request. One round-trip warms it up.
        await driver.evaluate('1');

        return {
            driver,
            process: child,
            rpcPort,
            close: teardown,
        };
    }
    catch (err)
    {
        await teardown();
        throw err;
    }
}

async function waitForPort(child: ChildProcess, timeoutMs: number,
                           logger?: { write(chunk: string): void }): Promise<number>
{
    let stdoutBuffer = '';
    let stderrBuffer = '';

    return await new Promise<number>((resolve, reject) =>
    {
        const onStdout = (chunk: Buffer): void =>
        {
            const text = chunk.toString('utf8');
            logger?.write(text);
            stdoutBuffer += text;

            const match = portLineRegex.exec(stdoutBuffer);
            if (match && match[1])
            {
                cleanup();
                resolve(Number.parseInt(match[1], 10));
            }
        };

        const onStderr = (chunk: Buffer): void =>
        {
            const text = chunk.toString('utf8');
            logger?.write(text);
            stderrBuffer += text;
        };

        const onExit = (code: number | null, signal: NodeJS.Signals | null): void =>
        {
            cleanup();
            reject(new Error(`test host exited before announcing port `
                             + `(code=${code}, signal=${signal})\n`
                             + `stdout: ${stdoutBuffer}\nstderr: ${stderrBuffer}`));
        };

        const onTimeout = (): void =>
        {
            cleanup();
            reject(new Error(`timed out after ${timeoutMs}ms waiting for `
                             + `EACP_RPC_PORT=<n> on stdout.\n`
                             + `stdout so far: ${stdoutBuffer}\n`
                             + `stderr so far: ${stderrBuffer}`));
        };

        const timer = setTimeout(onTimeout, timeoutMs);

        const cleanup = (): void =>
        {
            clearTimeout(timer);
            child.stdout?.off('data', onStdout);
            child.stderr?.off('data', onStderr);
            child.off('exit', onExit);
        };

        child.stdout?.on('data', onStdout);
        child.stderr?.on('data', onStderr);
        child.on('exit', onExit);
    });
}
