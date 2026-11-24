#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/UI/UI.h>

#include <cstdio>
#include <cstdlib>
#include <string>

// A process that is still showing a window when it exits, for ExitTests to run
// as a child and read the exit code and output of. argv[1] is what closes the
// window: "atexit" a handler registered before the window was made, "static" a
// namespace-scope object built before main - a host tearing its editor down
// from its own exit, after main has returned. Either one runs after the
// destructors of every function-local static made since, the backends' window,
// view and device singletons among them, so anything of those the close still
// reaches has to outlive the process.
//
// Headless unless EACP_HEADLESS is "0", so a desktop run opens nothing on the
// screen and a run inside a compositor opens a real window.

using namespace eacp;

namespace
{
struct Painted final : UI::Component
{
    void paint(UI::Graphics& g) override
    {
        g.setColour(UI::Color {0.2f, 0.4f, 0.8f, 1.f});
        g.fillRoundedRect(getLocalBounds(), 6.f);

        g.setColour(UI::Color {1.f, 1.f, 1.f, 1.f});
        g.drawText("exit", getLocalBounds(), UI::Justification::Centred);
    }
};

struct Editor
{
    Editor()
    {
        host.setRootComponent(root);

        auto options = Graphics::WindowOptions {};
        options.width = 240;
        options.height = 160;
        options.title = "ExitHarness";

        window = std::make_unique<Graphics::Window>(host, options);
        window->setVisible(true);

        // A frame through the shared device, so its process-wide half is up.
        host.renderToImage(1.f);
    }

    Painted root;
    UI::ComponentHost host;
    std::unique_ptr<Graphics::Window> window;
};

Editor* editor = nullptr;

void say(const char* text)
{
    std::printf("ExitHarness: %s\n", text);
    std::fflush(stdout);
}

void closeEditor()
{
    if (editor == nullptr)
        return;

    delete editor;
    editor = nullptr;
    say("closed");
}

struct ClosedByAStatic
{
    ~ClosedByAStatic() { closeEditor(); }
};

ClosedByAStatic closedByAStatic;
} // namespace

int main(int argc, char* argv[])
{
    const auto mode = std::string {argc > 1 ? argv[1] : ""};

    if (mode != "atexit" && mode != "static")
        return 2;

    if (getEnvValue("EACP_HEADLESS") != "0")
        Apps::setHeadless(true);

    if (mode == "atexit")
        std::atexit(closeEditor);

    return Apps::runConsole(argc,
                            argv,
                            []
                            {
                                editor = new Editor();
                                say("open");
                                return 0;
                            });
}
