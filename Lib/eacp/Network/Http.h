#pragma once

#include <map>
#include <string>

namespace eacp::HTTP
{
struct Request
{
    std::string url;
    std::string type = "GET";
    std::string body;
    std::map<std::string, std::string> headers;
};

struct Response
{
    std::string content;
    std::string error;
    int statusCode = 0;
};

Response httpRequest(const Request& req);
} // namespace eacp::HTTP
