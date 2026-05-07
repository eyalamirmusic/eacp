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

namespace
{
int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
} // namespace

std::string urlDecode(const std::string& encoded)
{
    auto result = std::string();
    result.reserve(encoded.size());

    for (auto i = size_t {0}; i < encoded.size(); ++i)
    {
        auto c = encoded[i];
        if (c == '+')
        {
            result.push_back(' ');
        }
        else if (c == '%' && i + 2 < encoded.size())
        {
            auto hi = hexValue(encoded[i + 1]);
            auto lo = hexValue(encoded[i + 2]);
            if (hi < 0 || lo < 0)
            {
                result.push_back(c);
                continue;
            }
            result.push_back((char) ((hi << 4) | lo));
            i += 2;
        }
        else
        {
            result.push_back(c);
        }
    }

    return result;
}

std::map<std::string, std::string> parseQueryString(const std::string& query)
{
    auto result = std::map<std::string, std::string>();

    auto pos = size_t {0};
    while (pos < query.size())
    {
        auto amp = query.find('&', pos);
        auto end = amp == std::string::npos ? query.size() : amp;
        auto pair = query.substr(pos, end - pos);

        if (!pair.empty())
        {
            auto eq = pair.find('=');
            if (eq == std::string::npos)
                result[urlDecode(pair)] = "";
            else
                result[urlDecode(pair.substr(0, eq))] =
                    urlDecode(pair.substr(eq + 1));
        }

        if (amp == std::string::npos)
            break;
        pos = amp + 1;
    }

    return result;
}
} // namespace eacp::HTTP