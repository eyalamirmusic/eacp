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

// Calls `onReadable` on the loop thread whenever `fd` is readable (or has hung
// up), from construction until destruction. Level-triggered: a callback that
// leaves data unread is called again on the next pump.
//
// The RAII form of addLoopSource, for descriptors that are not eacp's own: a
// VST3 host's IRunLoop::registerEventHandler, a plugin's display connection.
// Watchers never replace one another: several on one descriptor are each
// called, and destroying one leaves the rest watching, an addLoopSource entry
// for the same descriptor included.
//
// The fd is not owned: destroy the watcher before closing it. Construct and
// destroy it on the loop thread. Destroying any watcher from inside a callback
// is safe, and one destroyed earlier in a pump is not called later in it.
class FdWatcher
{
public:
    FdWatcher(int fdToWatch, Callback onReadable);
    ~FdWatcher();

    FdWatcher(const FdWatcher&) = delete;
    FdWatcher& operator=(const FdWatcher&) = delete;

    int getFd() const { return fd; }

private:
    int fd = -1;
    uint64_t id = 0;
};

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
