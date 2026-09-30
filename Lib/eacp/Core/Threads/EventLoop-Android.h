#pragma once

#include "EventLoop-Linux.h"

namespace eacp::Threads
{
using LooperEventHandler = std::function<void(int ident, void* data)>;

void setLooperEventHandler(LooperEventHandler handler);
} // namespace eacp::Threads
