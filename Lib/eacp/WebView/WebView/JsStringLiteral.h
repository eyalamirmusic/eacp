#pragma once

#include <string>
#include <string_view>

namespace eacp::Graphics
{

// Escapes a string as a double-quoted JS/JSON literal so it can be
// injected safely into evaluated script. Shared by the bridge wiring
// (Bridge.cpp) and the platform WebView glue (WebView-Shared.cpp).
std::string jsStringLiteral(std::string_view value);

} // namespace eacp::Graphics
