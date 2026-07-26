#pragma once

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Video/Player.h>
#include <NanoTest/NanoTest.h>

#include <string>

namespace VideoTests
{
// The same four clips the VideoViewDemo app plays, fetched by CMake
// into the shared media cache this macro points at.
inline eacp::FilePath clip(const std::string& name)
{
    return eacp::FilePath {std::string {EACP_VIDEO_TEST_MEDIA_DIR} + "/" + name};
}

// The Player delivers its notifications through callAsync, so tests pump the
// main-thread event loop while they wait.
template <typename Predicate>
bool waitFor(Predicate ready, int timeoutMs = 20000)
{
    return eacp::Threads::runEventLoopUntil(ready, eacp::Time::MS {timeoutMs});
}

inline void pumpFor(int ms)
{
    eacp::Threads::runEventLoopFor(eacp::Time::MS {ms});
}
} // namespace VideoTests
