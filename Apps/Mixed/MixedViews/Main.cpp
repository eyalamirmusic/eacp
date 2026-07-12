#include <eacp/CameraView/CameraView.h>
#include <eacp/WebView/WebView.h>
#include <algorithm>

#include <cstdlib>

using namespace eacp;

namespace
{
Graphics::WindowOptions makeOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 1280;
    options.height = 720;
    options.title = "eacp Mixed — Camera + transparent WebView controls";
    options.flags = {Graphics::WindowFlags::Titled,
                     Graphics::WindowFlags::Closable,
                     Graphics::WindowFlags::Miniaturizable,
                     Graphics::WindowFlags::Resizable};
    return options;
}

Graphics::Color
    hsvColor(float hueDegrees, float saturation, float value, float alpha)
{
    auto h = std::fmod(hueDegrees, 360.0f) / 60.0f;
    auto c = value * saturation;
    auto x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
    auto m = value - c;

    auto r = 0.0f, g = 0.0f, b = 0.0f;

    if (h < 1.0f)
    {
        r = c;
        g = x;
    }
    else if (h < 2.0f)
    {
        r = x;
        g = c;
    }
    else if (h < 3.0f)
    {
        g = c;
        b = x;
    }
    else if (h < 4.0f)
    {
        g = x;
        b = c;
    }
    else if (h < 5.0f)
    {
        r = x;
        b = c;
    }
    else
    {
        r = c;
        b = x;
    }

    return {r + m, g + m, b + m, alpha};
}

float parseFloat(const std::string& text, float fallback)
{
    char* end = nullptr;
    auto value = std::strtof(text.c_str(), &end);
    return end == text.c_str() ? fallback : value;
}

// The native GPU camera view, with overlay parameters driven live from the web
// sliders. tint washes the frame like a colour filter; the reticle is an
// AR-style marker whose size and orbit speed track their sliders. setMirrored
// (the camera view's own native control) is toggled from the web checkbox.
struct ControlledCameraView final : Cameras::CameraView
{
    void update(Threads::FrameTime frameTime) override
    {
        orbitPhase += orbitSpeed * static_cast<float>(frameTime.delta);

        if (autoQuitSeconds > 0.0 && frameTime.time >= autoQuitSeconds)
            Apps::quit();
    }

    void drawOverlay(Sprites::SpriteRenderer& renderer,
                     const Graphics::Rect& imageArea) override
    {
        auto area = (imageArea.w > 0.0f && imageArea.h > 0.0f) ? imageArea
                                                               : getLocalBounds();

        if (tintStrength > 0.0f)
            renderer.fillRect(area, hsvColor(tintHue, 0.85f, 1.0f, tintStrength));

        auto center = area.center();
        auto radius = 0.28f * std::min(area.w, area.h);
        auto bx = center.x + std::cos(orbitPhase) * radius;
        auto by = center.y + std::sin(orbitPhase) * radius;
        auto half = reticleSize * 0.5f;

        renderer.drawRect({bx - half, by - half, reticleSize, reticleSize},
                          {1.0f, 0.85f, 0.2f, 0.95f},
                          3.0f);
        renderer.drawLine({bx - half - 6.0f, by},
                          {bx + half + 6.0f, by},
                          {1.0f, 1.0f, 1.0f, 0.85f},
                          1.5f);
        renderer.drawLine({bx, by - half - 6.0f},
                          {bx, by + half + 6.0f},
                          {1.0f, 1.0f, 1.0f, 0.85f},
                          1.5f);
    }

    // Written from the web bridge (main thread), read here while rendering.
    float tintHue = 200.0f; // degrees
    float tintStrength = 0.0f; // overlay alpha, 0..1
    float reticleSize = 28.0f; // logical units
    float orbitSpeed = 1.2f; // radians / second
    float orbitPhase = 0.0f;
    double autoQuitSeconds = 0.0;
};

std::string controlPageHtml()
{
    return R"HTML(
<!doctype html><html><head><meta charset="utf-8"><style>
  :root { color-scheme: dark; }
  html, body { margin:0; height:100%; background:transparent; overflow:hidden;
               font-family:-apple-system, system-ui, sans-serif; }
  .panel { position:fixed; left:24px; bottom:24px; width:300px;
           padding:18px 20px 20px; border-radius:16px;
           background:rgba(18,20,28,0.55);
           backdrop-filter:blur(18px) saturate(140%);
           -webkit-backdrop-filter:blur(18px) saturate(140%);
           border:1px solid rgba(255,255,255,0.12);
           box-shadow:0 10px 40px rgba(0,0,0,0.45);
           color:#eaf2ff; user-select:none; }
  .title { font-size:14px; font-weight:600; }
  .subtitle { font-size:11px; color:#93a4c4; margin-bottom:14px; }
  .row { margin:14px 0; }
  .row label { display:flex; justify-content:space-between; font-size:12px;
               color:#c7d3ea; margin-bottom:6px; }
  .row .val { color:#7fe7b0; font-variant-numeric:tabular-nums; }
  input[type=range] { -webkit-appearance:none; appearance:none; width:100%;
                      height:4px; border-radius:4px;
                      background:rgba(255,255,255,0.18); outline:none; }
  input[type=range]::-webkit-slider-thumb { -webkit-appearance:none;
                      appearance:none; width:16px; height:16px; border-radius:50%;
                      background:#4f9fff; cursor:pointer; border:2px solid #0d1422;
                      box-shadow:0 1px 4px rgba(0,0,0,0.5); }
  .toggle { display:flex; align-items:center; justify-content:space-between;
            font-size:12px; color:#c7d3ea; }
  .toggle input { width:16px; height:16px; accent-color:#4f9fff; }
</style></head><body>
  <div class="panel">
    <div class="title">Camera controls</div>
    <div class="subtitle">HTML sliders driving the native GPU camera view</div>

    <div class="row">
      <label>Tint hue<span class="val" id="tintHue-val">200</span></label>
      <input id="tintHue" type="range" min="0" max="360" value="200">
    </div>
    <div class="row">
      <label>Tint strength<span class="val" id="tintStrength-val">0%</span></label>
      <input id="tintStrength" type="range" min="0" max="80" value="0">
    </div>
    <div class="row">
      <label>Reticle size<span class="val" id="reticleSize-val">28</span></label>
      <input id="reticleSize" type="range" min="8" max="80" value="28">
    </div>
    <div class="row">
      <label>Orbit speed<span class="val" id="orbitSpeed-val">1.20</span></label>
      <input id="orbitSpeed" type="range" min="0" max="300" value="120">
    </div>
    <div class="row toggle">
      <span>Mirror (flip horizontally)</span>
      <input id="mirror" type="checkbox" checked>
    </div>
  </div>
  <script>
    function post(name, value) {
      if (window.webkit && window.webkit.messageHandlers[name])
        window.webkit.messageHandlers[name].postMessage(String(value));
    }
    function setVal(id, text) {
      document.getElementById(id + '-val').textContent = text;
    }
    function bindSlider(id, format) {
      const el = document.getElementById(id);
      const handle = () => { setVal(id, format(el.value)); post(id, el.value); };
      el.addEventListener('input', handle);
      handle();
    }
    bindSlider('tintHue', v => v);
    bindSlider('tintStrength', v => v + '%');
    bindSlider('reticleSize', v => v);
    bindSlider('orbitSpeed', v => (v / 100).toFixed(2));

    const mirror = document.getElementById('mirror');
    const sendMirror = () => post('mirror', mirror.checked ? 1 : 0);
    mirror.addEventListener('change', sendMirror);
    sendMirror();
  </script>
</body></html>)HTML";
}

Graphics::WebView::Options transparentOptions()
{
    auto options = Graphics::WebView::Options {};
    options.transparentBackground = true;
    return options;
}

// Stacks the native camera view under a full-window transparent WebView: the
// camera shows through everywhere the page is transparent, and the page's
// floating slider panel is the only opaque region.
struct MixedRoot final : Graphics::View
{
    MixedRoot()
    {
        addChildren({cameraView, controls});

        cameraView.setMirrored(true); // front-camera-style preview by default
        wireBridge();
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        cameraView.setBounds(bounds);
        controls.setBounds(bounds);
    }

    void wireBridge()
    {
        controls.addScriptMessageHandler(
            "tintHue",
            [this](const std::string& v)
            { cameraView.tintHue = parseFloat(v, cameraView.tintHue); });

        controls.addScriptMessageHandler(
            "tintStrength",
            [this](const std::string& v)
            { cameraView.tintStrength = parseFloat(v, 0.0f) / 100.0f; });

        controls.addScriptMessageHandler(
            "reticleSize",
            [this](const std::string& v)
            { cameraView.reticleSize = parseFloat(v, cameraView.reticleSize); });

        controls.addScriptMessageHandler(
            "orbitSpeed",
            [this](const std::string& v)
            { cameraView.orbitSpeed = parseFloat(v, 0.0f) / 100.0f; });

        controls.addScriptMessageHandler("mirror",
                                         [this](const std::string& v)
                                         { cameraView.setMirrored(v == "1"); });

        controls.loadHTML(controlPageHtml(), "https://localhost/");
    }

    ControlledCameraView cameraView;
    Graphics::WebView controls {transparentOptions()};
};

struct MixedViewsApp
{
    MixedViewsApp()
    {
        root.cameraView.autoQuitSeconds =
            std::atof(getEnvValue("EACP_DEMO_AUTOQUIT_SECONDS").c_str());

        root.cameraView.attach(camera);
        window.setContentView(root);
        beginCapture();
    }

    ~MixedViewsApp() { camera.stop(); }

    void startCamera()
    {
        auto config = Cameras::CameraConfig {};
        config.width = 1280;
        config.height = 720;
        camera.start(config);
    }

    void beginCapture()
    {
        switch (Cameras::Camera::permissionStatus())
        {
            case Cameras::PermissionStatus::Granted:
                startCamera();
                break;
            case Cameras::PermissionStatus::NotDetermined:
                Cameras::Camera::requestPermission(
                    [this](bool granted)
                    {
                        if (granted)
                            startCamera();
                    });
                break;
            default:
                std::printf("Camera access not granted; showing overlay only.\n");
                break;
        }
    }

    Cameras::Camera camera;
    MixedRoot root;
    Graphics::Window window {makeOptions()};
};
} // namespace

int main()
{
    eacp::Apps::run<MixedViewsApp>();
    return 0;
}
