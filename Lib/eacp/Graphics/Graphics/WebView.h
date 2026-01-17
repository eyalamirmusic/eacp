#pragma once

#include "View.h"
#include <functional>
#include <string>

namespace eacp::Graphics
{
class WebView : public View
{
public:
    WebView();
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
