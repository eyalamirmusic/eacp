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

// The one descriptor a host loop watches for this eacp copy. Readable
// whenever pumpEventLoop() has something to do. Stable for the copy's life.
//
// Only a foreign host (a DAW) needs these two: when the host executable is
// itself an eacp app, the copy running its loop is found through the
// environment and this descriptor joins that loop by itself, on the first
// thing this copy defers (see attachCurrentThreadAsMain in EventLoop.h).
int getEventLoopFd();

// Runs every prepare, every ready source and every pending callback, and
// returns. Never blocks; safe to call when nothing is ready (LV2's idle).
void pumpEventLoop();
} // namespace eacp::Threads
