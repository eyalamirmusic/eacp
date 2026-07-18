#pragma once

#include "../Utils/Common.h"

namespace eacp::Clipboard
{
bool copyText(std::string_view text);
bool copyFiles(const Vector<std::string>& paths);

// The clipboard's text, or empty when it holds none — an image, a file promise,
// or nothing at all. Empty is also what a platform without a clipboard returns,
// so a caller never has to special-case that.
//
// Reads the system clipboard on every call rather than caching. Another
// application may have written to it since, and a stale paste is a worse
// failure than a slow one.
//
// Named to match the same function on jamierpond/eacp, which arrived first and
// already has callers; keeping two spellings of one idea in a young API is not
// worth the consistency with copyText.
std::string getText();

// Whether getText would return anything. Lets a Paste menu item be enabled
// without pulling the payload across, which for a large clipboard is not free.
bool hasText();
} // namespace eacp::Clipboard
