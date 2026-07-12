#include "TestAgent.h"

namespace eacp::WebView::Test
{

std::string loadTestAgentSource()
{
    auto view = ResEmbed::get("test-agent.js", "TestAgent");
    if (!view)
        throw std::runtime_error(
            "eacp-webview-test: embedded test-agent.js resource not found");
    return view.toString();
}

} // namespace eacp::WebView::Test
