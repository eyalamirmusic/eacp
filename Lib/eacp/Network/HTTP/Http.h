#pragma once

#include "../Common.h"

#include <atomic>
#include <map>

namespace eacp::HTTP
{

struct Response
{
    void setContent(const std::string& contentToUse, const std::string& contentType);
    void setHeader(const std::string& key, const std::string& value);
    void setRedirect(const std::string& url, int status = 302);

    std::string content;
    std::string error;
    std::map<std::string, std::string> headers;
    int statusCode = 0;
};

struct DownloadProgress
{
    std::atomic<std::int64_t> bytesReceived {0};
    std::atomic<std::int64_t> totalBytes {-1};
    std::atomic<bool> cancel {false};
    std::atomic<bool> done {false};
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

    bool hasHeader(const std::string& key) const;
    std::string getHeader(const std::string& key) const;

    bool hasParam(const std::string& key) const;
    std::string getParam(const std::string& key) const;

    std::string pathWithoutQuery() const;

    std::string url;
    std::string type = "GET";
    std::string body;
    std::map<std::string, std::string> headers;
    Vector<FormField> formFields;
    Vector<FileField> fileFields;

    std::map<std::string, std::string> params;
    std::string remoteAddr;
    int remotePort = -1;

    DownloadProgress* progress = nullptr;
    int parallelChunks = 1;
};

Response httpRequest(const Request& req);
Response downloadFile(const Request& req, const std::string& filePath);

std::string urlDecode(const std::string& encoded);
std::map<std::string, std::string> parseQueryString(const std::string& query);
} // namespace eacp::HTTP
