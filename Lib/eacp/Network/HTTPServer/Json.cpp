#include "Json.h"

namespace eacp::HTTP::Json
{

void throwError(const std::string& message, int statusCode)
{
    auto body = ErrorResponse {.error = message, .status = statusCode};
    auto response = Response();
    response.statusCode = statusCode;
    setJson(response, body);
    throw Error(std::move(response));
}

} // namespace eacp::HTTP::Json
