import './style.css';

const apiBase = 'http://localhost:8080';

const root = document.querySelector<HTMLDivElement>('#app')!;

root.innerHTML = `
    <h1>Hello from Vite + eacp</h1>
    <p>This page is served by Vite in dev and embedded via ResEmbed in release.</p>
    <button id="ping">Ping API</button>
    <div id="out"></div>
`;

const out = root.querySelector<HTMLDivElement>('#out')!;
const button = root.querySelector<HTMLButtonElement>('#ping')!;

interface PingResponse
{
    pong: boolean;
    serverTimeMs: number;
}

button.addEventListener('click', async () =>
{
    out.textContent = 'pinging...';
    button.disabled = true;
    try
    {
        const response = await fetch(`${apiBase}/api/ping`);
        if (!response.ok)
            throw new Error(`HTTP ${response.status}`);
        const data = (await response.json()) as PingResponse;
        const serverTime = new Date(data.serverTimeMs).toLocaleTimeString();
        out.textContent = `pong from server (server time: ${serverTime})`;
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
