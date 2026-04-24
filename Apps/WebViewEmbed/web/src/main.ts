import './style.css';

const root = document.querySelector<HTMLDivElement>('#app')!;

root.innerHTML = `
    <h1>Hello from Vite + eacp</h1>
    <p>This page is served by Vite in dev and embedded via ResEmbed in release.</p>
    <button id="ping">Ping</button>
    <div id="out"></div>
`;

const out = root.querySelector<HTMLDivElement>('#out')!;
root.querySelector<HTMLButtonElement>('#ping')!.addEventListener('click', () =>
{
    out.textContent = `clicked at ${new Date().toLocaleTimeString()}`;
});
