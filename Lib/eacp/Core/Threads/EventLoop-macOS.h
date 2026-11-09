#pragma once

namespace eacp::Threads::Detail
{
// [NSApp stop:] ends whatever modal session is on top rather than [NSApp run],
// so a quit that arrives while a runModal is up (an alert, a blocking file
// panel) is used up by the modal. Call this after every runModal: if this
// copy's quit is still pending at the root loop, it is put to the root loop
// again, and otherwise nothing happens. A quit from another eacp copy, or the
// host's own [NSApp stop:], is not this copy's to re-issue.
void resumeQuitAfterModal();

// Whether this copy's root loop has been asked to quit and has not yet ended:
// no time to put a modal in front of the user.
bool isRootQuitPending();
} // namespace eacp::Threads::Detail
