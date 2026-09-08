#include "LinuxWindowNative-Linux.h"

namespace eacp::Graphics
{
namespace
{
// Every window owns a backend, so a view on this one asks and is told no,
// rather than finding nothing to ask.
struct HeadlessViewSurfaceBackend final : ViewSurfaceBackend
{
    std::unique_ptr<ViewSurfaceNative> createSurface(View&, ViewSurface&) override
    {
        return {};
    }
};

// No window system under it, so the neutral state is the whole window: the
// content view is real and laid out, positions and titles round-trip, and
// nothing is ever mapped. A window whose compositor never answered ends up
// behaving like this too, by having no surface rather than by being this.
struct HeadlessWindowNative final
    : LinuxWindowNative
    , LinuxWindowSurface
{
    HeadlessWindowNative(const WindowOptions& options, WindowEvents& events)
        : state(*this, options, events)
    {
        viewSurfaces = std::make_unique<HeadlessViewSurfaceBackend>();
    }

    ~HeadlessWindowNative() override
    {
        if (contentView != nullptr)
            linuxUnbindWindowFromContentView(*contentView);
    }

    LinuxWindowState& getState() override { return state; }

    void* getHandle() override { return nullptr; }

    void setVisible(bool) override {}

    void setTitle(const std::string& newTitle) override { state.title = newTitle; }

    void minimize() override {}
    void toggleMaximize() override {}

    void setMouseLocked(bool locked) override { mouseLockIntent = locked; }

    bool isKeyPressed(uint16_t) override { return false; }
    ModifierKeys getModifiers() override { return {}; }

    LinuxWindowState state;
};
} // namespace

std::unique_ptr<LinuxWindowNative>
    makeHeadlessWindowNative(const WindowOptions& options, WindowEvents& events)
{
    return std::make_unique<HeadlessWindowNative>(options, events);
}
} // namespace eacp::Graphics
