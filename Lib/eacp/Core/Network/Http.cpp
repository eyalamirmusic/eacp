#include "Http.h"

namespace eacp::HTTP
{
Request::Request(const std::string& urlToUse)
    : url(urlToUse)
{
}

Request Request::post(const std::string& urlToUse, const std::string& bodyToUse)
{
    auto req = Request(urlToUse);
    req.type = "POST";
    req.body = bodyToUse;
    return req;
}
Response Request::perform() const
{
    return httpRequest(*this);
}

Response Request::downloadTo(const std::string& filePath) const
{
    return downloadFile(*this, filePath);
}
} // namespace eacp::HTTP