#pragma once

#include "../Utils/Common.h"

namespace eacp::Threads
{
class Timer
{
public:
    Timer(const Callback& cbToUse, int intervalHz);

private:
    Callback callback;

    struct Impl;
    std::shared_ptr<Impl> impl;
};
}

