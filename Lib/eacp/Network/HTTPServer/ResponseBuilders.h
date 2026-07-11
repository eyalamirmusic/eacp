#pragma once

#include "../HTTP/Http.h"

namespace eacp::HTTP
{

Response makePlainTextResponse(int statusCode, const std::string& message);

} // namespace eacp::HTTP
