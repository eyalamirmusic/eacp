#include "OnlineResourceInternal.h"

#include "../HTTP/HttpWeb.h"

#include <eacp/Core/Threads/EventLoop.h>

namespace eacp
{
bool onlineResourceFetchCanBlock()
{
    return HTTP::webHttpCanBlock();
}

void runOnlineResourceFetch(
    std::function<std::optional<OnlineResource::Result>()> prepare,
    HTTP::Request request,
    std::string partPath,
    std::function<OnlineResource::Result(const HTTP::Response&)> complete,
    std::function<void(OnlineResource::Result)> deliver)
{
    if (auto early = prepare())
    {
        Threads::callAsync([deliver, result = *early] { deliver(result); });
        return;
    }

    HTTP::startWebDownload(request,
                           partPath,
                           [complete, deliver](HTTP::Response response)
                           { deliver(complete(response)); });
}
} // namespace eacp
