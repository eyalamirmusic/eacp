#pragma once

#include <map>
#include <string>
#include <vector>

namespace eacp::HTTP
{

struct Response
{
    std::string content;
    std::string error;
    std::map<std::string, std::string> headers;
    int statusCode = 0;
};

struct FormField
{
    std::string name;
    std::string value;
};

struct FileField
{
    std::string fieldName;
    std::string filePath;
    std::string contentType = "application/octet-stream";
    std::string fileName;
};

struct Request
{
    Request(const std::string& urlToUse = "");

    static Request post(const std::string& urlToUse = "",
                        const std::string& bodyToUse = {});

    Request& addFormField(const std::string& name, const std::string& value);
    Request&
        addFileField(const std::string& fieldName,
                     const std::string& filePath,
                     const std::string& contentType = "application/octet-stream");

    Response perform() const;
    Response downloadTo(const std::string& filePath) const;

    std::string url;
    std::string type = "GET";
    std::string body;
    std::map<std::string, std::string> headers;
    std::vector<FormField> formFields;
    std::vector<FileField> fileFields;

    std::map<std::string, std::string> params;
    std::string remoteAddr;
    int remotePort = -1;
};

Response httpRequest(const Request& req);
Response downloadFile(const Request& req, const std::string& filePath);

std::string urlDecode(const std::string& encoded);
std::map<std::string, std::string> parseQueryString(const std::string& query);
} // namespace eacp::HTTP
