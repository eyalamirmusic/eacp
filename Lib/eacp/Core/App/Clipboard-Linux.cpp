#include "Clipboard.h"

namespace eacp::Clipboard
{
bool copyText(std::string_view)
{
    return false;
}

bool copyFiles(const Vector<std::string>&)
{
    return false;
}

std::string getText()
{
    return {};
}
} // namespace eacp::Clipboard
