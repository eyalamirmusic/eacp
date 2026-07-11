#pragma once

#include "../Common.h"

namespace eacp::WebView::Test
{

// Returns the source of the window.__test DOM-driving agent that
// AppDriver expects to be installed in the page. AppDriver's
// constructor calls this and feeds the result to addUserScript() at
// document-start, so the helpers are available before any page
// script runs. Test code generally doesn't need to call this
// directly.
//
// The agent itself lives at Resources/test-agent.js and is embedded
// into eacp-webview-test via res_embed_add().
std::string loadTestAgentSource();

} // namespace eacp::WebView::Test
