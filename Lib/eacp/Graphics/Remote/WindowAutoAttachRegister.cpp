// Compiled into the app binary (not into eacp-graphics-remote) by the
// debug-server cmake helper — a TU handed directly to the linker is
// never dead-stripped the way an unreferenced archive member is, so this
// initializer reliably runs before main() and installs the Window
// debug-attach hook.
#include "WindowAutoAttach.h"

namespace
{
[[maybe_unused]] const auto installed =
    eacp::Graphics::Remote::installWindowAutoAttach();
}
