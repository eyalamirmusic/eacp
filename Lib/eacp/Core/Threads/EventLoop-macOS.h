#pragma once

namespace eacp::Threads::Detail
{
// [NSApp stop:] ends whatever modal session is on top rather than [NSApp run],
// so a quit that arrives while a runModal is up (an alert, a blocking file
// panel) is used up by the modal. Call this once runModal has answered
// NSModalResponseStop: if this copy's quit is still pending at the root loop,
// it is put to the root loop again. Anything else that stopped the modal is
// left alone.
void resumeQuitAfterModal();
} // namespace eacp::Threads::Detail
