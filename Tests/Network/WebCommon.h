#pragma once

#include "Common.h"

#include <eacp/Network/HTTP/HttpProtocol.h>

#include <chrono>
#include <map>
#include <optional>

// The web cases talk to Tests/Network/Web/TestServer.mjs, which ctest starts
// as a fixture and which writes its addresses to EACP_WEB_TEST_SERVER_STATE: a
// wasm program cannot listen, so the in-process servers of the other suites
// cannot exist here. The suite links eacp-core-nested-pump, so its cases may
// block the main thread as they do elsewhere; the async cases wait the same
// way, in runEventLoopUntil.
namespace WebTests
{
using Clock = std::chrono::steady_clock;

struct ServerState
{
    std::string baseUrl;
    std::string otherBaseUrl;
    int pid = 0;

    MIRO_REFLECT(baseUrl, otherBaseUrl, pid)
};

struct Echo
{
    std::string method;
    std::string path;
    int port = 0;
    std::map<std::string, std::string> headers;
    int bodyLength = 0;
    std::string body;

    MIRO_REFLECT(method, path, port, headers, bodyLength, body)

    std::string header(const std::string& name) const
    {
        auto found = headers.find(name);
        return found == headers.end() ? std::string {} : found->second;
    }
};

inline const ServerState& server()
{
    static const auto state = []
    {
        auto loaded = ServerState {};
        Miro::fromJSONString(loaded, readFile(EACP_WEB_TEST_SERVER_STATE));
        return loaded;
    }();

    nano::check(!state.baseUrl.empty());
    return state;
}

inline std::string url(const std::string& path)
{
    return server().baseUrl + path;
}

inline std::string webSocketUrl(const std::string& path)
{
    return "ws" + server().baseUrl.substr(4) + path;
}

inline Echo echoOf(const eacp::HTTP::Response& response)
{
    auto echo = Echo {};
    Miro::fromJSONString(echo, response.content);
    return echo;
}

inline std::string pattern(int offset, int size)
{
    auto bytes = std::string((std::size_t) size, '\0');

    for (auto i = 0; i < size; ++i)
        bytes[(std::size_t) i] = (char) ((offset + i) % 251);

    return bytes;
}

inline void writeBytes(const std::string& path, const std::string& bytes)
{
    auto out = std::ofstream(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

inline long long millisecondsSince(Clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()
                                                                 - start)
        .count();
}

// Runs the callbacks the runtime runs as it exits, without exiting it
// (HttpWebTests-Web.cpp).
void runTheRuntimeExitCallbacks();

template <typename T>
bool waitFor(const std::optional<T>& value)
{
    return eacp::Threads::runEventLoopUntil([&] { return value.has_value(); },
                                            fetchTimeout);
}
} // namespace WebTests
