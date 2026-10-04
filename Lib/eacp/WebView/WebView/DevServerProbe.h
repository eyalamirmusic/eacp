#pragma once

#include <string>

namespace eacp::Graphics
{
bool probeDevServer(const std::string& url, int timeoutMs);
}
