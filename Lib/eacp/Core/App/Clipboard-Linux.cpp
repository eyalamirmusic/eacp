#include "Clipboard-Linux.h"

namespace eacp::Clipboard
{
namespace
{
// A plain static: every entry point here runs on the message thread, and so
// does the backend's installation.
Backend& clipboardBackend()
{
    static auto backend = Backend {};
    return backend;
}
} // namespace

void setBackend(Backend backend)
{
    clipboardBackend() = std::move(backend);
}

void clearBackend()
{
    clipboardBackend() = Backend {};
}

bool copyText(std::string_view text)
{
    return clipboardBackend().copyText(text);
}

// Empty is the documented answer for a platform with no clipboard, which is
// what a Linux build with no windowing backend is, so callers need no special
// case.
std::string getText()
{
    return clipboardBackend().getText();
}

bool hasText()
{
    return clipboardBackend().hasText();
}

bool copyFiles(const Vector<std::string>& paths)
{
    return clipboardBackend().copyFiles(paths);
}
} // namespace eacp::Clipboard
