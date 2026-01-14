#pragma once

#include <string>
#include "Primitives.h"
#include "../Utils/Common.h"

namespace eacp::Graphics
{

class Font
{
public:
    Font();
    Font(const std::string& fontName, float size);

    ~Font() = default;

    void setSize(float size);
    float getSize() const;

    void* getHandle() const;

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
