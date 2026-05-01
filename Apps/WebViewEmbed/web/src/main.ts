import './style.css';

interface PingResponse
{
    pong: boolean;
    serverTimeMs: number;
}

interface EacpBridge
{
    invoke<Res = unknown, Req = unknown>(command: string, payload?: Req): Promise<Res>;
    on<T = unknown>(event: string, handler: (payload: T) => void): () => void;
}

declare global
{
    interface Window
    {
        eacp: EacpBridge;
    }
}

const root = document.querySelector<HTMLDivElement>('#app')!;

root.innerHTML = `
    <h1>Hello from Vite + eacp</h1>
    <p>This page talks to native code through the eacp WebView bridge.</p>
    <button id="ping">Ping native</button>
    <div id="out"></div>
`;

const out = root.querySelector<HTMLDivElement>('#out')!;
const button = root.querySelector<HTMLButtonElement>('#ping')!;

button.addEventListener('click', async () =>
{
    out.textContent = 'pinging...';
    button.disabled = true;
    try
    {
        const data = await window.eacp.invoke<PingResponse>('ping');
        const serverTime = new Date(data.serverTimeMs).toLocaleTimeString();
        out.textContent = `pong from native (server time: ${serverTime})`;
    }
    catch (err)
    {
        out.textContent = `error: ${err instanceof Error ? err.message : err}`;
    }
    finally
    {
        button.disabled = false;
    }
});
