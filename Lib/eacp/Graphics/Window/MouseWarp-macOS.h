#pragma once

namespace eacp::Graphics::detail
{
// Warping the cursor moves it without the user having moved it, and the next
// mouse event still reports that jump in its delta — as though the hand had
// made it. For a locked window that delta is the whole width of the window in
// one frame, which snaps the camera round. Engaging a mouse lock warps the
// cursor into the window, so it raises this; the mouse event that carries the
// phantom motion clears it and drops its delta.
//
// A process has one cursor, so one flag covers every window.
inline bool cursorWasWarped = false;
} // namespace eacp::Graphics::detail
