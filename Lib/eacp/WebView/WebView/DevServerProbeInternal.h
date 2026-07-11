#pragma once

#include "../Common.h"

namespace eacp::Graphics
{
bool probeTCP(const std::string& host, int port, int timeoutMs);
}
