#pragma once

#include <eacp/Graphics/Graphics.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace eacp::Graphics
{
struct ResourceResponse
{
    std::string mimeType;
    std::vector<std::uint8_t> data;
    int statusCode = 200;
};

using ResourceProvider =
    std::function<std::optional<ResourceResponse>(std::string_view url)>;

class WebView : public View
{
public:
    struct Options
    {
        std::unordered_map<std::string, ResourceProvider> schemes;
    };

    WebView();
    explicit WebView(Options options);
    ~WebView() override;

    void loadURL(const std::string& url);
    void loadHTML(const std::string& html, const std::string& baseURL = "");

    void goBack();
    void goForward();
    void reload();
    void stopLoading();

    bool canGoBack() const;
    bool canGoForward() const;
    bool isLoading() const;

    std::string getURL() const;
    std::string getTitle() const;

    using JSCallback =
        std::function<void(const std::string& result, const std::string& error)>;
    void evaluateJavaScript(const std::string& script,
                            JSCallback callback = nullptr);

    void addScriptMessageHandler(
        const std::string& name,
        std::function<void(const std::string& message)> handler);
    void removeScriptMessageHandler(const std::string& name);

    std::function<void(const std::string& url)> onNavigationStarted;
    std::function<void(const std::string& url)> onNavigationFinished;
    std::function<void(const std::string& error)> onNavigationFailed;
    std::function<void(const std::string& title)> onTitleChanged;

protected:
    void resized() override;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Graphics
