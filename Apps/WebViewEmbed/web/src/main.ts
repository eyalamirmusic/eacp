import './style.css';
import {backend} from './generated/backend';

const root = document.querySelector<HTMLDivElement>('#app')!;

root.innerHTML = `
    <h1>Hello from Vite + eacp</h1>
    <p>This page talks to native code through the eacp WebView bridge.</p>
    <button id="ping">Ping native</button>
    <div id="out"></div>
`;

const out = root.querySelector<HTMLDivElement>('#out')!;
const button = root.querySelector<HTMLButtonElement>('#ping')!;

button.addEventListener('click', async () => {
    out.textContent = 'pinging...';
    const data = await backend.ping();
    const serverTime = new Date(data.serverTimeMs).toLocaleTimeString();
    out.textContent = `pong from native (server time: ${serverTime})`;

});
