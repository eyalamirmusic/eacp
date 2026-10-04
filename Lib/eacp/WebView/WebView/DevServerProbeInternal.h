#pragma once

#include <string>

namespace eacp::Graphics
{
bool probeTCP(const std::string& host, int port, int timeoutMs);
}
