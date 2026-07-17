#pragma once

#include <functional>
#include <string>

namespace term::Notifier
{
// Sets up the platform notification center and the click handler: activating
// a delivered notification calls onActivate (on the main thread) with the
// session key it was posted for. Call once at startup.
void initialize(std::function<void(const std::string& sessionKey)> onActivate);

// Posts a desktop notification attributed to a session. Clicking it brings
// the app forward and jumps to that session.
void notify(const std::string& sessionKey,
            const std::string& title,
            const std::string& body);
} // namespace term::Notifier
