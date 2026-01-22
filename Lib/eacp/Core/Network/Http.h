#pragma once

#include <map>
#include <string>

namespace eacp::HTTP
{

struct Response
{
    std::string content;
    std::string error;
    int statusCode = 0;
};

struct Request
{
    Request(const std::string& urlToUse = "");

    static Request post(const std::string& urlToUse = "",
                        const std::string& bodyToUse = {});

    Response perform() const;

    std::string url;
    std::string type = "GET";
    std::string body;
    std::map<std::string, std::string> headers;
};

Response httpRequest(const Request& req);
} // namespace eacp::HTTP
