#pragma once

#include "OnlineResource.h"

#include "../HTTP/Http.h"

#include <functional>
#include <optional>

namespace eacp
{
// Where an OnlineResource fetch meets the platform's way of waiting for a
// transfer. prepare runs first and returns a result when the fetch ends
// before any request (cancelled, or the directory cannot be made); otherwise
// request is downloaded to partPath and complete turns its response into the
// result. deliver is called on the main thread with the result either way,
// once.
void runOnlineResourceFetch(
    std::function<std::optional<OnlineResource::Result>()> prepare,
    HTTP::Request request,
    std::string partPath,
    std::function<OnlineResource::Result(const HTTP::Response&)> complete,
    std::function<void(OnlineResource::Result)> deliver);

// Whether the blocking OnlineResource::fetch can wait for its fetch here:
// always where the loop can be pumped from inside a call, and on the web only
// in an executable that can suspend.
bool onlineResourceFetchCanBlock();
} // namespace eacp
