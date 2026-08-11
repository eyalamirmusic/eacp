#pragma once

#include "../Common.h"

namespace eacp::WebView::Test
{

// Source of the window.__test agent (Resources/test-agent.js), which
// AppDriver installs for itself — tests rarely need to call this.
std::string loadTestAgentSource();

} // namespace eacp::WebView::Test
