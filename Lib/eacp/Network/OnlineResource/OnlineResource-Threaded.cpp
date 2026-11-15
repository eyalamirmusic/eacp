#include "OnlineResourceInternal.h"

#include <eacp/Core/Threads/EventLoop.h>

#include <thread>

namespace eacp
{
bool onlineResourceFetchCanBlock()
{
    return true;
}

void runOnlineResourceFetch(
    std::function<std::optional<OnlineResource::Result>()> prepare,
    HTTP::Request request,
    std::string partPath,
    std::function<OnlineResource::Result(const HTTP::Response&)> complete,
    std::function<void(OnlineResource::Result)> deliver)
{
    std::thread {
        [prepare = std::move(prepare),
         request = std::move(request),
         partPath = std::move(partPath),
         complete = std::move(complete),
         deliver = std::move(deliver)]
        {
            auto early = prepare();
            auto result =
                early ? std::move(*early) : complete(request.downloadTo(partPath));

            Threads::callAsync([deliver, output = std::move(result)]() mutable
                               { deliver(std::move(output)); });
        }}
        .detach();
}
} // namespace eacp
