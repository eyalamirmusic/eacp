#include "Font.h"

namespace eacp::Graphics
{
FontOptions FontOptions::withName(const std::string& newName) const
{
    auto copy = *this;
    copy.name = newName;
    return copy;
}

FontOptions FontOptions::withSize(float newSize)
{
    auto copy = *this;
    copy.size = newSize;
    return copy;
}
} // namespace eacp::Graphics
