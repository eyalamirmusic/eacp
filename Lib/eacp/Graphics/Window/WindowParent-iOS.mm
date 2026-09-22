#include "Window.h"

namespace eacp::Graphics::detail
{
// UIKit has one window, it fills the screen, and there is no second one to
// dock to it. WindowOptions::parent compiles here and does nothing, exactly as
// initialPosition and setVisible do.
void attachNativeParent(Window&, Window*)
{
}

bool backendCarriesChildWindows()
{
    return false;
}
} // namespace eacp::Graphics::detail
