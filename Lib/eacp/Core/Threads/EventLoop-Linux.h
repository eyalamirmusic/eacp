#pragma once

#include "EventLoop.h"

namespace eacp::Threads
{
// A descriptor the message loop polls alongside its own waker. The fd is not
// owned: remove it before closing it. Registering one twice replaces its entry.
void addLoopSource(int fd, short events, Callback callback);

// The same, with `prepare` run immediately before every poll(2).
void addLoopSource(int fd, short events, Callback callback, Callback prepare);

void removeLoopSource(int fd);
} // namespace eacp::Threads
