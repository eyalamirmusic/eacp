#include <eacp/Graphics/Graphics.h>

using namespace eacp;
using namespace eacp::Graphics;

// A window on the page's canvas that prints what reaches its view: presses
// with their click count, drags, the wheel and keys. Nothing is drawn, since
// the web has no 2D Context yet; the canvas shows the page behind it.
struct InputLogView : View
{
    InputLogView()
    {
        setHandlesMouseEvents(true);
        setMouseCursor(MouseCursor::Crosshair);
    }

    void mouseDown(const MouseEvent& event) override
    {
        log("down", event, Strings::concat(" clicks ", event.clickCount));
        focus();
    }

    void mouseUp(const MouseEvent& event) override { log("up", event); }

    void mouseDragged(const MouseEvent& event) override
    {
        log("drag",
            event,
            Strings::concat(" by ", event.delta.x, ",", event.delta.y));
    }

    void mouseWheel(const MouseEvent& event) override
    {
        log("wheel",
            event,
            Strings::concat(" by ",
                            event.delta.x,
                            ",",
                            event.delta.y,
                            event.preciseScrolling ? " points" : " lines"));
    }

    void keyDown(const KeyEvent& event) override
    {
        Console::print(Strings::concat("key ",
                                       event.keyCode,
                                       " '",
                                       event.characters,
                                       event.isRepeat ? "' repeat" : "'"));
    }

    void resized() override
    {
        auto bounds = getBounds();
        Console::print(Strings::concat("size ", bounds.w, "x", bounds.h));
    }

    void backingScaleChanged() override
    {
        Console::print(Strings::concat("scale ", primaryDisplay().backingScale));
    }

    static void
        log(const char* what, const MouseEvent& event, std::string extra = {})
    {
        Console::print(
            Strings::concat(what, " at ", event.pos.x, ",", event.pos.y, extra));
    }
};

int main()
{
    auto options = WindowOptions {};
    options.title = "Hello Input";

    return runWindowedApp<InputLogView>(options);
}
