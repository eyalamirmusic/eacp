#pragma once

#include <eacp/Core/Utils/Common.h>

namespace eacp::Threads
{
class DisplayLink
{
public:
    explicit DisplayLink(const Callback& cbToUse);

private:
    Callback callback;

    struct Native;
    Pimpl<Native> impl;
};
}
