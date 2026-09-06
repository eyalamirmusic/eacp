#pragma once

#include "EventLoop.h"

namespace eacp::Threads
{
// A descriptor the message loop watches alongside its own waker.
//
// Linux has no run-loop object to attach a source to, so anything with a
// pollable fd — a Wayland or xcb display connection, an inotify watch, a
// timerfd — joins the pump by handing the descriptor over here. That is what
// keeps eacp-core free of libwayland: the loop knows about poll(2), and the
// library that owns the connection knows about the protocol.
//
// `events` is a poll(2) event mask, POLLIN for "there is something to read".
// The callback runs on the loop thread from inside run() / runFor(), before
// the callAsync queue is drained, so work it posts is picked up in the same
// turn. Nesting still works: a callback may pump the loop again with
// runEventLoopFor, and the source stays live inside that nested pump.
//
// The descriptor is not owned. Remove it before closing it, or poll() will
// keep reporting a closed fd. Registering the same fd twice replaces the
// previous entry rather than adding a second one, and removing one that was
// never added does nothing.
//
// Linux-only, and deliberately in a header no other platform includes: macOS
// and Windows have run loops that already accept native sources of their own.
void addLoopSource(int fd, short events, Callback callback);

// The same, with a second callback run on the loop thread immediately before
// every poll(2) — before the poll set is even built, so a prepare that adds or
// removes sources is honoured on that same turn.
//
// What needs it is a connection whose traffic does not all travel over the
// descriptor at the moment the loop looks. A Wayland client writes its
// requests into libwayland's own output buffer, and they reach the compositor
// only at wl_display_flush — so a loop that blocks in poll() without flushing
// first is waiting for a reply to a request it has not sent. The mirror case
// is inbound: another thread reading the same connection (Mesa's Vulkan WSI
// does exactly that from inside present) leaves events queued in memory with
// nothing left on the fd, and poll() then sleeps over a queue that is already
// full.
//
// Both are fixed by a few lines that have to run before the wait rather than
// after it, which is the one place a readable-descriptor callback cannot
// reach. Sources registered without a prepare behave exactly as before.
void addLoopSource(int fd, short events, Callback callback, Callback prepare);

void removeLoopSource(int fd);
} // namespace eacp::Threads
