#pragma once

#include "WebView.h"

#include <Miro/Miro.h>
#include <functional>
#include <string>

namespace eacp::Graphics
{

using EmptyMessage = Miro::EmptyValue;

class WebViewBridge
{
public:
    explicit WebViewBridge(WebView& webView);
    ~WebViewBridge();

    template <typename Req, typename Res>
    void on(const std::string& command, std::function<Res(const Req&)> handler)
    {
        commands.on<Req, Res>(command, std::move(handler));
    }

    template <typename Req, typename Res>
    void on(const std::string& command, Res (*handler)(const Req&))
    {
        commands.on<Req, Res>(command, handler);
    }

    template <typename T>
    void send(const std::string& event, const T& payload)
    {
        dispatchEvent(event, Miro::toJSON(payload));
    }

private:
    void onMessage(const std::string& body);
    void deliver(double id,
                 const Miro::Json::Value& result,
                 const std::string* error);
    void dispatchEvent(const std::string& event, const Miro::Json::Value& payload);

    WebView& webView;
    Miro::CommandTable commands;
};

} // namespace eacp::Graphics
