#pragma once

namespace eacp::Graphics::detail
{
// Raised when a mouse lock warps the cursor, so the next mouse event drops the
// phantom jump it reports as delta. One cursor per process, so one flag.
inline bool cursorWasWarped = false;
} // namespace eacp::Graphics::detail
