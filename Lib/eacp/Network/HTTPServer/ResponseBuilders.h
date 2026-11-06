#pragma once

/** @file Shorthand for building common HTTP responses. */

#include "../HTTP/Http.h"

namespace eacp::HTTP
{

Response makePlainTextResponse(int statusCode, const std::string& message);

} // namespace eacp::HTTP
