#pragma once

#include <string>
#include "Primitives.h"
#include <eacp/Core/Utils/Common.h>

namespace eacp::Graphics
{

struct FontOptions
{
    FontOptions withName(const std::string& newName) const
    {
        auto copy = *this;
        copy.name = newName;
        return copy;
    }

    FontOptions withSize(float newSize)
    {
        auto copy = *this;
        copy.size = newSize;
        return copy;
    }

    std::string name = "Helvetica";
    float size = 12.f;
};

class Font
{
public:
    Font(const FontOptions& optionsToUse = {});
    ~Font() = default;

    void setFont(const FontOptions& optionsToUse);
    void* getHandle() const;

private:
    void updateNativeFont();

    FontOptions options;

    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
