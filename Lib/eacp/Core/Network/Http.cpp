#include "Http.h"
#include <fstream>
#include <random>
#include <sstream>

namespace eacp::HTTP
{

std::string generateBoundary()
{
    auto rd = std::random_device();
    auto gen = std::mt19937(rd());
    auto dist = std::uniform_int_distribution<>(0, 15);
    auto chars = "0123456789abcdef";

    auto ss = std::stringstream();
    ss << "----eacp";
    for (int i = 0; i < 16; i++)
        ss << chars[dist(gen)];

    return ss.str();
}

std::string filenameFromPath(const std::string& path)
{
    auto pos = path.find_last_of("/\\");
    if (pos != std::string::npos)
        return path.substr(pos + 1);
    return path;
}

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

Request& Request::addFormField(const std::string& name, const std::string& value)
{
    formFields.push_back({name, value});
    type = "POST";
    return *this;
}

Request& Request::addFileField(const std::string& fieldName,
                               const std::string& filePath,
                               const std::string& contentType)
{
    auto fileName = filenameFromPath(filePath);
    fileFields.push_back({fieldName, filePath, contentType, fileName});
    type = "POST";
    return *this;
}

void buildMultipartBody(Request& req)
{
    if (req.formFields.empty() && req.fileFields.empty())
        return;

    auto boundary = generateBoundary();
    req.headers["Content-Type"] = "multipart/form-data; boundary=" + boundary;

    auto ss = std::stringstream();

    for (const auto& field: req.formFields)
    {
        ss << "--" << boundary << "\r\n";
        ss << "Content-Disposition: form-data; name=\"" << field.name
           << "\"\r\n\r\n";
        ss << field.value << "\r\n";
    }

    for (const auto& file: req.fileFields)
    {
        auto stream = std::ifstream(file.filePath, std::ios::binary);
        auto content = std::string(std::istreambuf_iterator<char>(stream),
                                   std::istreambuf_iterator<char>());

        ss << "--" << boundary << "\r\n";
        ss << "Content-Disposition: form-data; name=\"" << file.fieldName
           << "\"; filename=\"" << file.fileName << "\"\r\n";
        ss << "Content-Type: " << file.contentType << "\r\n\r\n";
        ss << content << "\r\n";
    }

    ss << "--" << boundary << "--\r\n";
    req.body = ss.str();
}

Response Request::perform() const
{
    auto prepared = *this;
    buildMultipartBody(prepared);
    return httpRequest(prepared);
}

Response Request::downloadTo(const std::string& filePath) const
{
    return downloadFile(*this, filePath);
}
} // namespace eacp::HTTP