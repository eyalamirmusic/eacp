#pragma once

#include "WebView.h"
#include <eacp/Graphics/Window/Window.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace eacp::Graphics
{

class WindowPool
{
public:
    WindowPool();
    ~WindowPool();

    WindowPool(const WindowPool&) = delete;
    WindowPool& operator=(const WindowPool&) = delete;

    Window& emplaceBackContentView(View& view);
    WebView& emplaceBackWebView();

    bool empty() const;
    std::size_t size() const;

    std::function<void(Window&)> onWindowWillClose;

    static WindowPool* active();

private:
    struct Entry
    {
        std::unique_ptr<WebView> ownedWebView;
        std::unique_ptr<Window> window;
    };

    void handleClose(Entry* entry);

    std::vector<std::unique_ptr<Entry>> entries;
};

} // namespace eacp::Graphics
