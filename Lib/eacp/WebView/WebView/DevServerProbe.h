#pragma once

/** @file Checking whether a development server is reachable. */

#include <string>

namespace eacp::Graphics
{
/// Blocks up to `timeoutMs` trying a TCP connect to the URL's host and port; true if it connects. Sends nothing.
bool probeDevServer(const std::string& url, int timeoutMs);
}
