#pragma once

#include <eacp/Graphics/Graphics.h>
#include <functional>
#include <string>

namespace eacp::Graphics
{

struct NavigationRequest
{
    std::string url;
};

enum class NavigationDecision
{
    Allow,
    Block,
    OpenExternally,
};

struct NewWindowRequest
{
    std::string url;
};

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

    using NewWindowHandler =
        std::function<WebView*(const NewWindowRequest& request)>;

    std::function<void(const std::string& url)> onNavigationStarted;
    std::function<void(const std::string& url)> onNavigationFinished;
    std::function<void(const std::string& error)> onNavigationFailed;
    std::function<void(const std::string& title)> onTitleChanged;

    std::function<NavigationDecision(const NavigationRequest& request)>
        onNavigationDecision;
    NewWindowHandler onNewWindowRequested;

protected:
    void resized() override;

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
