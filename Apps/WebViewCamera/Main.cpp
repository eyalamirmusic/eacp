#include <eacp/WebView/WebView.h>

using namespace eacp;
using namespace Graphics;

namespace
{
constexpr auto cameraPage = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8" />
<title>WebView Camera</title>
<style>
    body {
        font-family: -apple-system, sans-serif;
        margin: 0;
        padding: 1.5rem;
        background: #1e1e28;
        color: #e6e6f0;
    }
    h1 { margin: 0 0 0.75rem; font-size: 1.25rem; }
    p  { color: #b0b0c0; margin: 0 0 1rem; }
    button {
        background: #4c6ef5;
        color: white;
        border: 0;
        padding: 0.6rem 1.1rem;
        border-radius: 6px;
        font-size: 1rem;
        cursor: pointer;
    }
    button:disabled { background: #555; cursor: default; }
    button + button { margin-left: 0.5rem; }
    video {
        display: block;
        margin-top: 1rem;
        width: 100%;
        max-width: 640px;
        background: black;
        border-radius: 8px;
    }
    #status { margin-top: 0.75rem; font-family: monospace; color: #cfd0e2; }
    #level {
        margin-top: 0.5rem;
        height: 12px;
        background: #2a2a35;
        border-radius: 6px;
        overflow: hidden;
        max-width: 640px;
    }
    #level > div {
        height: 100%;
        width: 0%;
        background: linear-gradient(90deg, #4c6ef5, #f59f00, #fa5252);
        transition: width 60ms linear;
    }
</style>
</head>
<body>
    <h1>getUserMedia from a WebView</h1>
    <p>The camera and microphone are opened from JavaScript via
       <code>navigator.mediaDevices.getUserMedia</code>. macOS will prompt for
       permission the first time.</p>

    <button id="start">Start camera + mic</button>
    <button id="stop" disabled>Stop</button>

    <video id="preview" autoplay playsinline muted></video>
    <div id="level"><div></div></div>
    <div id="status">Idle.</div>

    <script>
        const startBtn = document.getElementById('start');
        const stopBtn  = document.getElementById('stop');
        const video    = document.getElementById('preview');
        const status   = document.getElementById('status');
        const levelBar = document.querySelector('#level > div');

        let stream = null;
        let audioCtx = null;
        let analyser = null;
        let rafId = null;

        function setStatus(msg) { status.textContent = msg; }

        function updateLevel()
        {
            const buf = new Uint8Array(analyser.fftSize);
            analyser.getByteTimeDomainData(buf);
            let peak = 0;
            for (const v of buf) peak = Math.max(peak, Math.abs(v - 128));
            levelBar.style.width = Math.min(100, (peak / 128) * 200) + '%';
            rafId = requestAnimationFrame(updateLevel);
        }

        async function start()
        {
            startBtn.disabled = true;
            setStatus('Requesting camera and microphone...');
            try
            {
                stream = await navigator.mediaDevices.getUserMedia({
                    video: true,
                    audio: true,
                });
                video.srcObject = stream;

                audioCtx = new (window.AudioContext || window.webkitAudioContext)();
                const src = audioCtx.createMediaStreamSource(stream);
                analyser = audioCtx.createAnalyser();
                analyser.fftSize = 1024;
                src.connect(analyser);
                updateLevel();

                stopBtn.disabled = false;
                setStatus('Streaming.');
            }
            catch (err)
            {
                startBtn.disabled = false;
                setStatus('Error: ' + err.name + ' — ' + err.message);
            }
        }

        function stop()
        {
            if (rafId) cancelAnimationFrame(rafId);
            rafId = null;
            if (stream) stream.getTracks().forEach(t => t.stop());
            stream = null;
            if (audioCtx) audioCtx.close();
            audioCtx = null;
            video.srcObject = null;
            levelBar.style.width = '0%';
            startBtn.disabled = false;
            stopBtn.disabled = true;
            setStatus('Stopped.');
        }

        startBtn.addEventListener('click', start);
        stopBtn.addEventListener('click', stop);
    </script>
</body>
</html>
)HTML";
} // namespace

struct MyApp
{
    MyApp()
    {
        setApplicationMenuBar(buildDefaultWebViewMenuBar());
        window.setContentView(webView);
        webView.loadHTML(cameraPage, "https://localhost/");
    }

    WebView webView;
    Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();
    return 0;
}
