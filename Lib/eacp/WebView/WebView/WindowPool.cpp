#include "WindowPool.h"

#include <eacp/Core/App/App.h>

#include <algorithm>

namespace eacp::Graphics
{

namespace
{
WindowPool*& activePool()
{
    static WindowPool* instance = nullptr;
    return instance;
}
} // namespace

WindowPool::WindowPool()
{
    activePool() = this;
    onWindowWillClose = [this](Window&)
    {
        if (empty())
            Apps::quit();
    };
}

WindowPool::~WindowPool()
{
    if (activePool() == this)
        activePool() = nullptr;
}

WindowPool* WindowPool::active() { return activePool(); }

bool WindowPool::empty() const { return entries.empty(); }
std::size_t WindowPool::size() const { return entries.size(); }

Window& WindowPool::emplaceBackContentView(View& view)
{
    auto entry = std::make_unique<Entry>();
    auto* raw = entry.get();

    WindowOptions options;
    options.onClose = [this, raw] { handleClose(raw); };

    entry->window = std::make_unique<Window>(options);
    entry->window->setContentView(view);

    auto& windowRef = *entry->window;
    entries.push_back(std::move(entry));
    return windowRef;
}

WebView& WindowPool::emplaceBackWebView()
{
    auto entry = std::make_unique<Entry>();
    entry->ownedWebView = std::make_unique<WebView>();
    auto* raw = entry.get();

    WindowOptions options;
    options.onClose = [this, raw] { handleClose(raw); };

    entry->window = std::make_unique<Window>(options);
    entry->window->setContentView(*entry->ownedWebView);

    auto& webViewRef = *entry->ownedWebView;
    entries.push_back(std::move(entry));
    return webViewRef;
}

void WindowPool::handleClose(Entry* entry)
{
    auto it = std::find_if(entries.begin(), entries.end(),
                           [&](const auto& e) { return e.get() == entry; });

    if (it == entries.end())
        return;

    auto closing = std::move(*it);
    entries.erase(it);

    if (onWindowWillClose && closing->window)
        onWindowWillClose(*closing->window);
}

} // namespace eacp::Graphics
