#include <eacp/Graphics/Graphics.h>
#include <eacp/Graphics/View/View-Linux.h>

#include <emscripten/emscripten.h>

#include <cmath>
#include <optional>

// A window on a real <canvas> in a page, driven by DOM events the harness
// dispatches on it: the canvas, its sizes and cursor, the pointer, wheel and
// key events a view receives, a presenting view's Kind::Web surface and its
// frames, and the DisplayLink. Under node there is no page, and the same
// window must come up headless: laid out, never shown. Exits 0 when every
// check held.

using namespace eacp;
using namespace eacp::Graphics;

namespace
{
// clang-format off
EM_JS(int, harnessHasDom, (), {
    return typeof document !== 'undefined' ? 1 : 0;
});

EM_JS(double, harnessDevicePixelRatio, (), {
    return typeof devicePixelRatio === 'number' ? devicePixelRatio : 1;
});

EM_JS(int, harnessCanvasAttribute, (const char* selector, int which), {
    const canvas = document.querySelector(UTF8ToString(selector));
    if (!(canvas instanceof HTMLCanvasElement)) return -1;
    switch (which) {
        case 0: return canvas.width;
        case 1: return canvas.height;
        case 2: return parseInt(canvas.style.width);
        case 3: return parseInt(canvas.style.height);
        case 4: return canvas.style.display === 'none' ? 0 : 1;
        default: return -1;
    }
});

EM_JS(int, harnessCanvasCursorIs, (const char* selector, const char* cursor), {
    const canvas = document.querySelector(UTF8ToString(selector));
    return canvas && canvas.style.cursor === UTF8ToString(cursor) ? 1 : 0;
});

EM_JS(int, harnessCanvasExists, (const char* selector), {
    return document.querySelector(UTF8ToString(selector)) ? 1 : 0;
});

EM_JS(void, harnessPointer, (const char* selector, const char* type, double x,
                             double y, int button, int buttons), {
    const canvas = document.querySelector(UTF8ToString(selector));
    const rect = canvas.getBoundingClientRect();
    canvas.dispatchEvent(new PointerEvent(UTF8ToString(type), {
        bubbles: true, cancelable: true, pointerId: 1, pointerType: 'mouse',
        clientX: rect.left + x, clientY: rect.top + y, button, buttons}));
});

EM_JS(void, harnessWheel, (const char* selector, double x, double y,
                           double deltaY, int deltaMode), {
    const canvas = document.querySelector(UTF8ToString(selector));
    const rect = canvas.getBoundingClientRect();
    canvas.dispatchEvent(new WheelEvent('wheel', {
        bubbles: true, cancelable: true, clientX: rect.left + x,
        clientY: rect.top + y, deltaY, deltaMode}));
});

EM_JS(int, harnessKey, (const char* selector, const char* type, const char* code,
                        const char* key, int shift), {
    const canvas = document.querySelector(UTF8ToString(selector));
    const event = new KeyboardEvent(UTF8ToString(type), {
        bubbles: true, cancelable: true, code: UTF8ToString(code),
        key: UTF8ToString(key), shiftKey: shift !== 0});
    canvas.dispatchEvent(event);
    return event.defaultPrevented ? 1 : 0;
});
// clang-format on

struct RecordingView : View
{
    RecordingView() { setHandlesMouseEvents(true); }

    void mouseDown(const MouseEvent& event) override { mice.add(event); }
    void mouseUp(const MouseEvent& event) override { mice.add(event); }
    void mouseDragged(const MouseEvent& event) override { mice.add(event); }
    void mouseMoved(const MouseEvent& event) override { mice.add(event); }
    void mouseWheel(const MouseEvent& event) override { mice.add(event); }

    void keyDown(const KeyEvent& event) override { keys.add(event); }

    void keyUp(const KeyEvent& event) override
    {
        keys.add(event);
        passKeyOn();
    }

    std::optional<MouseEvent> last(MouseEventType type) const
    {
        for (auto index = (int) mice.size() - 1; index >= 0; --index)
            if (mice[index].type == type)
                return mice[index];

        return std::nullopt;
    }

    Vector<MouseEvent> mice;
    Vector<KeyEvent> keys;
};

WindowOptions harnessOptions()
{
    auto options = WindowOptions {};
    options.title = "WebWindowHarness";
    options.width = 400;
    options.height = 300;
    return options;
}
} // namespace

struct WebWindowHarness
{
    WebWindowHarness()
    {
        view.addSubview(presenter);
        presenter.setBounds({0.f, 0.f, 100.f, 100.f});

        auto& record = requestViewSurface(presenter);
        record.onAvailable = [this] { surfaceAvailable(); };
        record.onFrameDone = [this] { ++surfaceFrames; };

        if (harnessHasDom() != 0)
            checkCanvas();
        else
            checkHeadless();

        Threads::callAfter(Time::MS {10000}, [this] { finish("timed out"); });
    }

    void check(bool condition, const char* what)
    {
        Console::print(Strings::concat(condition ? "ok   " : "FAIL ", what));

        if (!condition)
            ++failures;
    }

    void checkHeadless()
    {
        Console::print("no document: the window is headless");

        check(window.getHandle() == nullptr, "a headless window has no canvas");
        check(!window.isVisible(), "a headless window is never shown");
        check(window.getSize().x == 400.f && window.getSize().y == 300.f,
              "a headless window keeps its size");

        window.setSize({320.f, 200.f});
        check(window.getSize().x == 320.f, "a headless window resizes");
        check(view.getBounds().w == 320.f, "the content follows the size");

        startDisplayLink();
    }

    void checkCanvas()
    {
        selector = static_cast<const char*>(window.getHandle());
        auto scale = harnessDevicePixelRatio();

        check(selector != nullptr, "the window has a canvas");

        if (selector == nullptr)
            return finish("no canvas");

        Console::print(Strings::concat("canvas ", selector, " at ", scale, "x"));

        check(window.isVisible(), "the window is shown once it has content");
        check(canvasAttribute(4) == 1, "the canvas is displayed");
        check(canvasAttribute(2) == 400 && canvasAttribute(3) == 300,
              "the canvas's CSS size is the window's");
        check(canvasAttribute(0) == (int) std::lround(400 * scale),
              "the canvas's width is the CSS width times devicePixelRatio");

        checkPointer();
        checkWheel();
        checkKeys();
        checkCursor();

        window.setSize({320.f, 200.f});
        check(canvasAttribute(2) == 320 && canvasAttribute(3) == 200,
              "setSize sets the CSS size");
        check(canvasAttribute(0) == (int) std::lround(320 * scale),
              "setSize sets the backing size");
        check(view.getBounds().w == 320.f, "the content follows the size");
    }

    void checkPointer()
    {
        harnessPointer(selector, "pointermove", 50, 40, 0, 0);
        auto moved = view.last(MouseEventType::Moved);
        check(moved && moved->pos.x == 50.f && moved->pos.y == 40.f,
              "a move reaches the view at its position");

        harnessPointer(selector, "pointerdown", 50, 40, 0, 1);
        harnessPointer(selector, "pointerup", 50, 40, 0, 0);
        harnessPointer(selector, "pointerdown", 51, 40, 0, 1);

        auto down = view.last(MouseEventType::Down);
        check(down && down->clickCount == 2, "a quick second press counts 2");

        harnessPointer(selector, "pointermove", 70, 45, 0, 1);
        auto dragged = view.last(MouseEventType::Dragged);
        check(dragged && dragged->delta.x == 19.f && dragged->delta.y == 5.f,
              "a move with the button held is a drag with its delta");

        harnessPointer(selector, "pointerup", 70, 45, 0, 0);
        auto up = view.last(MouseEventType::Up);
        check(up && up->button == MouseButton::Left && up->downPos.x == 51.f,
              "the release carries the press's position");

        harnessPointer(selector, "pointerdown", 20, 20, 2, 2);
        down = view.last(MouseEventType::Down);
        check(down && down->button == MouseButton::Right,
              "the right button is Right");
        harnessPointer(selector, "pointerup", 20, 20, 2, 0);
    }

    void checkWheel()
    {
        harnessWheel(selector, 30, 30, 100, 0);
        auto wheel = view.last(MouseEventType::Wheel);
        check(wheel && wheel->preciseScrolling && wheel->delta.y == -100.f,
              "a pixel wheel is precise points, positive towards the start");

        harnessWheel(selector, 30, 30, 3, 1);
        wheel = view.last(MouseEventType::Wheel);
        check(wheel && !wheel->preciseScrolling && wheel->delta.y == -3.f,
              "a line wheel is notches");
    }

    void checkKeys()
    {
        auto kept = harnessKey(selector, "keydown", "KeyA", "A", 1);
        check(!view.keys.empty() && view.keys.back().keyCode == KeyCode::A,
              "a key reaches the view with its KeyCode");
        check(!view.keys.empty() && view.keys.back().characters == "A",
              "the key carries its text");
        check(kept == 1, "a key the view kept prevents the page's default");
        check(Keyboard::isKeyPressed(KeyCode::A) && Keyboard::isShiftPressed(),
              "the polled state holds the key while it is down");

        kept = harnessKey(selector, "keyup", "KeyA", "A", 1);
        check(kept == 0, "a key the view passed on is left to the page");
        check(!Keyboard::isKeyPressed(KeyCode::A),
              "the polled state lets go on release");
    }

    void checkCursor()
    {
        view.setMouseCursor(MouseCursor::PointingHand);
        harnessPointer(selector, "pointermove", 60, 60, 0, 0);
        check(harnessCanvasCursorIs(selector, "pointer") == 1,
              "the view's cursor is the canvas's CSS cursor");
    }

    void surfaceAvailable()
    {
        auto& record = requestViewSurface(presenter);

        check(record.handle.kind == NativeSurfaceHandle::Kind::Web,
              "a presenting view's surface is Kind::Web");
        check(record.handle.surface != nullptr
                  && Strings::concat(static_cast<const char*>(record.handle.surface))
                         == selector,
              "its surface is the canvas's selector");
        check(record.handle.window != 0, "its window id is not zero");
        check(record.pixelWidth == canvasAttribute(0),
              "its pixel size is the canvas's");

        record.requestFrameCallback();
        startDisplayLink();
    }

    void startDisplayLink()
    {
        link.emplace(
            [this]
            {
                if (++ticks == 3)
                    finishTicking();
            });
    }

    void finishTicking()
    {
        check(ticks >= 3, "the DisplayLink ticks");

        if (selector != nullptr)
        {
            check(surfaceFrames == 1, "a frame request is answered once");

            auto copy = std::string(selector);
            closeWindow();
            check(harnessCanvasExists(copy.c_str()) == 0,
                  "a window's own canvas goes with it");
        }

        finish(nullptr);
    }

    void closeWindow()
    {
        view.removeSubview(presenter);
        windowHolder.reset();
    }

    int canvasAttribute(int which) const
    {
        return harnessCanvasAttribute(selector, which);
    }

    void finish(const char* reason)
    {
        if (finished)
            return;

        finished = true;
        link.reset();

        if (reason != nullptr)
            check(false, reason);

        Console::print(failures == 0 ? "PASS" : "FAIL");
        Apps::quit(failures == 0 ? 0 : 1);
    }

    RecordingView view;
    View presenter;
    std::optional<Window> windowHolder {std::in_place, view, harnessOptions()};
    Window& window = *windowHolder;

    const char* selector = nullptr;
    std::optional<Threads::DisplayLink> link;
    int ticks = 0;
    int surfaceFrames = 0;
    int failures = 0;
    bool finished = false;
};

int main()
{
    return Apps::run<WebWindowHarness>();
}
