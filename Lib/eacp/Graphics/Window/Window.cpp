#include "Window.h"

namespace eacp::Graphics
{
Window::Window(View& view, const WindowOptions& optionsToUse)
    : Window(optionsToUse)
{
    setContentView(view);
}
} // namespace eacp::Graphics
