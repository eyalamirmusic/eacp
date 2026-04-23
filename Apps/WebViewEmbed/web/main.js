document.getElementById('ping').addEventListener('click', () => {
    const out = document.getElementById('out');
    out.textContent = `main.js ran at ${new Date().toLocaleTimeString()}`;
});
