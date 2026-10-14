#include "Http.h"
#include "HttpProtocol.h"
#include <algorithm>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <system_error>
#include <thread>

namespace eacp::HTTP
{

void Response::setContent(const std::string& contentToUse,
                          const std::string& contentType)
{
    content = contentToUse;
    headers["Content-Type"] = contentType;
}

void Response::setHeader(const std::string& key, const std::string& value)
{
    headers[key] = value;
}

void Response::setRedirect(const std::string& url, int status)
{
    statusCode = status;
    headers["Location"] = url;
}

bool Request::hasHeader(const std::string& key) const
{
    for (auto& [k, v]: headers)
        if (Strings::equalsCaseInsensitive(k, key))
            return true;
    return false;
}

std::string Request::getHeader(const std::string& key) const
{
    return findHeaderIgnoringCase(headers, key);
}

bool Request::hasParam(const std::string& key) const
{
    return params.contains(key);
}

std::string Request::getParam(const std::string& key) const
{
    auto it = params.find(key);
    return it == params.end() ? std::string() : it->second;
}

std::string Request::pathWithoutQuery() const
{
    auto q = url.find('?');
    if (q == std::string::npos)
        return url;
    return url.substr(0, q);
}

namespace
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

void appendFormFields(std::stringstream& body,
                      const std::string& boundary,
                      const Vector<FormField>& fields)
{
    for (const auto& field: fields)
    {
        body << "--" << boundary << "\r\n";
        body << "Content-Disposition: form-data; name=\"" << field.name
             << "\"\r\n\r\n";
        body << field.value << "\r\n";
    }
}

std::string fileFieldContent(const FileField& file)
{
    if (file.inMemory)
        return file.content;

    auto stream = std::ifstream(file.filePath, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

void appendFileFields(std::stringstream& body,
                      const std::string& boundary,
                      const Vector<FileField>& files)
{
    for (const auto& file: files)
    {
        auto content = fileFieldContent(file);

        body << "--" << boundary << "\r\n";
        body << "Content-Disposition: form-data; name=\"" << file.fieldName
             << "\"; filename=\"" << file.fileName << "\"\r\n";
        body << "Content-Type: " << file.contentType << "\r\n\r\n";
        body << content << "\r\n";
    }
}

void buildMultipartBody(Request& req)
{
    if (req.formFields.empty() && req.fileFields.empty())
        return;

    auto boundary = generateBoundary();
    req.headers["Content-Type"] = "multipart/form-data; boundary=" + boundary;

    auto body = std::stringstream();
    appendFormFields(body, boundary, req.formFields);
    appendFileFields(body, boundary, req.fileFields);
    body << "--" << boundary << "--\r\n";

    req.body = body.str();
}
} // namespace

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
    formFields.add({name, value});
    type = "POST";
    return *this;
}

FileField FileField::fromBytes(const std::string& fieldName,
                               const std::string& fileName,
                               std::string bytes,
                               const std::string& contentType)
{
    auto field = FileField();
    field.fieldName = fieldName;
    field.fileName = fileName;
    field.contentType = contentType;
    field.content = std::move(bytes);
    field.inMemory = true;
    return field;
}

Request& Request::addFileField(const std::string& fieldName,
                               const std::string& filePath,
                               const std::string& contentType)
{
    auto field = FileField();
    field.fieldName = fieldName;
    field.filePath = filePath;
    field.contentType = contentType;
    field.fileName = Files::filenameFromPath(filePath);

    fileFields.add(std::move(field));
    type = "POST";
    return *this;
}

Request& Request::addFileBytes(const std::string& fieldName,
                               const std::string& fileName,
                               std::string bytes,
                               const std::string& contentType)
{
    fileFields.add(
        FileField::fromBytes(fieldName, fileName, std::move(bytes), contentType));
    type = "POST";
    return *this;
}

Response Request::perform() const
{
    auto prepared = *this;
    buildMultipartBody(prepared);
    return httpRequest(prepared);
}

namespace
{
constexpr std::int64_t kMinParallelTotal = 1024 * 1024;
constexpr int kMaxParallelChunks = 8;

struct ChunkRange
{
    std::int64_t start;
    std::int64_t end;
};

struct RangeProbe
{
    bool supportsRanges = false;
    std::int64_t totalBytes = -1;
    std::map<std::string, std::string> headers;
};

RangeProbe probeRangeSupport(const Request& req)
{
    auto head = req;
    head.type = "HEAD";
    head.parallelChunks = 1;
    head.progress = nullptr;
    head.headers.erase("Range");
    preferIdentityEncoding(head);

    auto response = httpRequest(head);

    auto probe = RangeProbe();
    probe.headers = response.headers;

    auto succeeded = response.error.empty() && response.statusCode >= 200
                     && response.statusCode < 300;

    if (!succeeded)
        return probe;

    probe.supportsRanges =
        acceptsByteRanges(findHeaderIgnoringCase(response.headers, "Accept-Ranges"));

    if (!probe.supportsRanges)
        return probe;

    auto contentLength = findHeaderIgnoringCase(response.headers, "Content-Length");
    if (contentLength.empty())
        return probe;

    try
    {
        probe.totalBytes = std::stoll(contentLength);
    }
    catch (...)
    {
        probe.totalBytes = -1;
    }

    return probe;
}

Vector<ChunkRange> computeChunkRanges(std::int64_t total, int chunkCount)
{
    auto chunkSize = total / chunkCount;
    auto ranges = Vector<ChunkRange>(chunkCount);

    for (auto i = 0; i < chunkCount; ++i)
    {
        ranges[i].start = i * chunkSize;
        ranges[i].end = (i == chunkCount - 1) ? total - 1 : (i + 1) * chunkSize - 1;
    }

    return ranges;
}

Vector<std::string> makeChunkPaths(const std::string& filePath, int chunkCount)
{
    auto paths = Vector<std::string>(chunkCount);

    for (auto i = 0; i < chunkCount; ++i)
        paths[i] = filePath + ".part" + std::to_string(i);

    return paths;
}

void removeChunkFiles(const Vector<std::string>& chunkPaths)
{
    for (const auto& path: chunkPaths)
    {
        auto ec = std::error_code();
        std::filesystem::remove(path, ec);
    }
}

std::string downloadChunk(const Request& sourceReq,
                          const ChunkRange& range,
                          DownloadProgress& progress,
                          const std::string& destPath)
{
    auto worker = sourceReq;
    worker.parallelChunks = 1;
    worker.resumeDownload = false;
    worker.type = "GET";
    worker.progress = &progress;
    worker.headers["Range"] =
        "bytes=" + std::to_string(range.start) + "-" + std::to_string(range.end);
    preferIdentityEncoding(worker);

    auto res = downloadFile(worker, destPath);

    if (!res.error.empty())
        return res.error;

    if (res.statusCode != 206)
        return "Range request returned status " + std::to_string(res.statusCode);

    return {};
}

class ErrorRecorder
{
public:
    void record(std::string message)
    {
        auto lock = std::lock_guard(mutex);
        if (firstError.empty())
            firstError = std::move(message);
    }

    std::string take()
    {
        auto lock = std::lock_guard(mutex);
        return firstError;
    }

private:
    std::mutex mutex;
    std::string firstError;
};

std::int64_t sumReceivedBytes(const OwnedVector<DownloadProgress>& chunkProgress)
{
    auto sum = std::int64_t(0);
    for (auto i = 0; i < chunkProgress.size(); ++i)
        sum += chunkProgress[i]->bytesReceived.load();
    return sum;
}

void propagateCancelToChunks(OwnedVector<DownloadProgress>& chunkProgress)
{
    for (auto i = 0; i < chunkProgress.size(); ++i)
        chunkProgress[i]->cancel.store(true);
}

std::thread launchProgressAggregator(DownloadProgress* aggregate,
                                     OwnedVector<DownloadProgress>& chunkProgress,
                                     std::atomic<bool>& shouldStop)
{
    return std::thread(
        [aggregate, &chunkProgress, &shouldStop]
        {
            while (!shouldStop.load())
            {
                if (aggregate)
                {
                    aggregate->bytesReceived.store(sumReceivedBytes(chunkProgress));

                    if (aggregate->cancel.load())
                        propagateCancelToChunks(chunkProgress);
                }
                Time::sleepMS(25);
            }
        });
}

Vector<std::thread> launchChunkWorkers(const Request& sourceReq,
                                       const Vector<ChunkRange>& ranges,
                                       OwnedVector<DownloadProgress>& progress,
                                       const Vector<std::string>& chunkPaths,
                                       ErrorRecorder& errors)
{
    auto threads = Vector<std::thread>();
    threads.reserve(ranges.size());

    for (auto i = 0; i < ranges.size(); ++i)
    {
        threads.emplace_back(
            [&sourceReq, &ranges, &progress, &chunkPaths, &errors, i]
            {
                auto message =
                    downloadChunk(sourceReq, ranges[i], *progress[i], chunkPaths[i]);

                if (!message.empty())
                    errors.record(std::move(message));
            });
    }

    return threads;
}

std::string concatenateChunkFiles(const std::string& filePath,
                                  const Vector<std::string>& chunkPaths)
{
    auto out = std::ofstream(filePath, std::ios::binary | std::ios::trunc);

    if (!out)
        return "Failed to open destination file";

    for (const auto& path: chunkPaths)
    {
        auto in = std::ifstream(path, std::ios::binary);

        if (!in)
            return "Failed to read chunk file";

        out << in.rdbuf();
    }

    out.close();

    if (!out)
        return "Failed to write destination file";

    return {};
}

bool shouldUseSingleStreamDownload(const RangeProbe& probe, int requestedChunks)
{
    auto chunkCount = std::min(requestedChunks, kMaxParallelChunks);
    return !probe.supportsRanges || probe.totalBytes < kMinParallelTotal
           || chunkCount < 2;
}

Response makeParallelFailureResponse(const std::string& error,
                                     const Vector<std::string>& chunkPaths)
{
    removeChunkFiles(chunkPaths);
    auto response = Response();
    response.error = error;
    return response;
}
} // namespace

Response downloadFileParallel(const Request& req, const std::string& filePath)
{
    auto probe = probeRangeSupport(req);

    if (shouldUseSingleStreamDownload(probe, req.parallelChunks))
        return downloadFile(req, filePath);

    auto chunkCount = std::min(req.parallelChunks, kMaxParallelChunks);
    auto ranges = computeChunkRanges(probe.totalBytes, chunkCount);
    auto chunkPaths = makeChunkPaths(filePath, chunkCount);

    if (req.progress)
        req.progress->totalBytes.store(probe.totalBytes);

    auto chunkProgress = OwnedVector<DownloadProgress>();
    for (auto i = 0; i < chunkCount; ++i)
        chunkProgress.createNew();
    auto errors = ErrorRecorder();
    auto workers =
        launchChunkWorkers(req, ranges, chunkProgress, chunkPaths, errors);

    auto stopAggregator = std::atomic<bool>(false);
    auto aggregator =
        launchProgressAggregator(req.progress, chunkProgress, stopAggregator);

    for (auto& t: workers)
        t.join();

    stopAggregator.store(true);
    aggregator.join();

    if (req.progress)
        req.progress->bytesReceived.store(sumReceivedBytes(chunkProgress));

    auto error = errors.take();
    if (req.progress && req.progress->cancel.load() && error.empty())
        error = "cancelled";

    if (!error.empty())
        return makeParallelFailureResponse(error, chunkPaths);

    auto mergeError = concatenateChunkFiles(filePath, chunkPaths);
    removeChunkFiles(chunkPaths);

    if (!mergeError.empty())
    {
        auto response = Response();
        response.error = mergeError;
        return response;
    }

    auto response = Response();
    response.statusCode = 200;
    response.headers = probe.headers;
    return response;
}

namespace
{
std::int64_t existingFileSize(const std::string& filePath)
{
    auto ec = std::error_code();
    auto size = std::filesystem::file_size(filePath, ec);
    return ec ? 0 : (std::int64_t) size;
}

std::int64_t bytesToResumeFrom(const Request& req, const std::string& filePath)
{
    if (!req.resumeDownload || req.hasHeader("Range"))
        return 0;

    return existingFileSize(filePath);
}

Request withRangeFrom(const Request& req, std::int64_t offset)
{
    auto ranged = req;
    ranged.headers["Range"] = "bytes=" + std::to_string(offset) + "-";
    preferIdentityEncoding(ranged);
    return ranged;
}

// A 206 that stops short of the end - a server that caps how much one range
// returns - leaves a file that is a correct but incomplete prefix, kept so a
// later resume can carry on from it.
std::string incompleteRemainderError(const Response& response,
                                     const std::string& filePath)
{
    auto range =
        parseContentRange(findHeaderIgnoringCase(response.headers, "Content-Range"));

    if (range.total < 0 || existingFileSize(filePath) == range.total)
        return {};

    return "The server sent bytes " + std::to_string(range.first) + "-"
           + std::to_string(range.last) + " of " + std::to_string(range.total)
           + "; the download is incomplete";
}

Response finishAppended(Response response, const std::string& filePath)
{
    response.error = incompleteRemainderError(response, filePath);
    response.statusCode = response.error.empty() ? 200 : 0;
    return response;
}

bool fileIsAlreadyWhole(const Response& response, std::int64_t fileSize)
{
    auto range =
        parseContentRange(findHeaderIgnoringCase(response.headers, "Content-Range"));
    return response.statusCode == 416 && range.total == fileSize;
}

bool restartsFromScratch(const Response& response)
{
    return response.statusCode == 416 || response.statusCode == 206;
}

void reportWholeFile(DownloadProgress* progress, std::int64_t fileSize)
{
    if (progress == nullptr)
        return;

    progress->bytesReceived.store(fileSize);
    progress->totalBytes.store(fileSize);
}

void resetProgress(DownloadProgress* progress)
{
    if (progress == nullptr)
        return;

    progress->bytesReceived.store(0);
    progress->totalBytes.store(-1);
}

Response downloadResuming(const Request& req, const std::string& filePath)
{
    auto offset = bytesToResumeFrom(req, filePath);

    if (offset <= 0)
        return downloadFileFrom(req, filePath, 0);

    auto response = downloadFileFrom(withRangeFrom(req, offset), filePath, offset);

    if (!response.error.empty())
        return response;

    auto write = downloadWriteFor(response.statusCode, response.headers, offset);

    if (write == DownloadWrite::Append)
        return finishAppended(std::move(response), filePath);

    if (write == DownloadWrite::Replace)
        return response;

    if (fileIsAlreadyWhole(response, offset))
    {
        reportWholeFile(req.progress, offset);
        response.statusCode = 200;
        response.content.clear();
        return response;
    }

    if (!restartsFromScratch(response))
        return response;

    resetProgress(req.progress);
    return downloadFileFrom(req, filePath, 0);
}

bool resumesExistingFile(const Request& req, const std::string& filePath)
{
    return bytesToResumeFrom(req, filePath) > 0;
}
} // namespace

Response downloadFile(const Request& req, const std::string& filePath)
{
    auto response = downloadResuming(req, filePath);

    if (req.progress)
        req.progress->done.store(true);

    return response;
}

Response Request::downloadTo(const std::string& filePath) const
{
    if (parallelChunks > 1 && !resumesExistingFile(*this, filePath))
    {
        auto res = downloadFileParallel(*this, filePath);
        if (progress)
            progress->done.store(true);
        return res;
    }
    return downloadFile(*this, filePath);
}

namespace
{
void appendDecodedPercentEscape(std::string& out, const std::string& src, int& i)
{
    auto hi = Strings::hexCharToInt(src[(std::size_t) i + 1]);
    auto lo = Strings::hexCharToInt(src[(std::size_t) i + 2]);

    if (hi < 0 || lo < 0)
    {
        out.push_back('%');
        return;
    }

    out.push_back((char) ((hi << 4) | lo));
    i += 2;
}
} // namespace

namespace
{
bool isAsciiAlphanumeric(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
           || (c >= '0' && c <= '9');
}

bool isUnreservedUrlChar(unsigned char c)
{
    return isAsciiAlphanumeric(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

void appendPercentEscape(std::string& out, unsigned char c)
{
    constexpr auto digits = std::string_view("0123456789ABCDEF");

    out.push_back('%');
    out.push_back(digits[c >> 4]);
    out.push_back(digits[c & 0x0F]);
}
} // namespace

std::string urlEncode(const std::string& text)
{
    auto result = std::string();
    result.reserve(text.size());

    for (auto c: text)
    {
        if (isUnreservedUrlChar((unsigned char) c))
            result.push_back(c);
        else
            appendPercentEscape(result, (unsigned char) c);
    }

    return result;
}

std::string urlDecode(const std::string& encoded)
{
    auto result = std::string();
    result.reserve(encoded.size());

    for (auto i = 0; i < (int) encoded.size(); ++i)
    {
        auto c = encoded[(std::size_t) i];

        if (c == '+')
            result.push_back(' ');
        else if (c == '%' && i + 2 < (int) encoded.size())
            appendDecodedPercentEscape(result, encoded, i);
        else
            result.push_back(c);
    }

    return result;
}

namespace
{
void parseQueryPair(const std::string& pair, Vector<QueryParameter>& out)
{
    if (pair.empty())
        return;

    auto eq = pair.find('=');
    if (eq == std::string::npos)
        out.add({urlDecode(pair), {}});
    else
        out.add({urlDecode(pair.substr(0, eq)), urlDecode(pair.substr(eq + 1))});
}
} // namespace

Vector<QueryParameter> parseQueryParameters(const std::string& query)
{
    auto result = Vector<QueryParameter>();
    auto pos = std::size_t {0};

    while (pos < query.size())
    {
        auto amp = query.find('&', pos);
        auto end = amp == std::string::npos ? query.size() : amp;
        parseQueryPair(query.substr(pos, end - pos), result);

        if (amp == std::string::npos)
            break;
        pos = amp + 1;
    }

    return result;
}

std::map<std::string, std::string> parseQueryString(const std::string& query)
{
    auto result = std::map<std::string, std::string>();

    for (auto& parameter: parseQueryParameters(query))
        result[parameter.name] = parameter.value;

    return result;
}

std::string eacpUserAgent()
{
#ifdef EACP_VERSION_STRING
    return std::string("eacp/") + EACP_VERSION_STRING;
#else
    return "eacp";
#endif
}

namespace
{
struct UserAgentOverride
{
    std::mutex mutex;
    std::string agent;
};

UserAgentOverride& userAgentOverride()
{
    static auto instance = UserAgentOverride();
    return instance;
}

// NSURLSession and HttpURLConnection name the app and system themselves
// when a request adds no User-Agent; WinHTTP needs an agent to open a
// session, and curl sends none at all.
std::string platformFallbackUserAgent()
{
#if defined(__APPLE__) || defined(__ANDROID__)
    return {};
#else
    return eacpUserAgent();
#endif
}
} // namespace

void setDefaultUserAgent(const std::string& agent)
{
    auto& state = userAgentOverride();
    auto lock = std::scoped_lock(state.mutex);
    state.agent = agent;
}

std::string defaultUserAgent()
{
    {
        auto& state = userAgentOverride();
        auto lock = std::scoped_lock(state.mutex);

        if (!state.agent.empty())
            return state.agent;
    }

    return platformFallbackUserAgent();
}

namespace
{
std::string querySeparator(const std::string& base)
{
    if (base.find('?') == std::string::npos)
        return "?";

    return base.ends_with("?") || base.ends_with("&") ? "" : "&";
}
} // namespace

URL::URL(const std::string& text)
{
    auto hash = text.find('#');
    auto withoutFragment = text.substr(0, hash);
    auto question = withoutFragment.find('?');
    auto query = question == std::string::npos
                     ? std::string()
                     : withoutFragment.substr(question + 1);

    base = query.empty() ? withoutFragment : withoutFragment.substr(0, question);
    parameters = parseQueryParameters(query);

    if (hash != std::string::npos)
        fragment = text.substr(hash + 1);
}

URL URL::parse(const std::string& text)
{
    return URL(text);
}

URL& URL::addParameter(const std::string& name, const std::string& value)
{
    parameters.add({name, value});
    return *this;
}

std::string URL::getParameter(const std::string& name) const
{
    for (auto& parameter: parameters)
        if (parameter.name == name)
            return parameter.value;

    return {};
}

bool URL::hasParameter(const std::string& name) const
{
    for (auto& parameter: parameters)
        if (parameter.name == name)
            return true;

    return false;
}

std::string URL::query() const
{
    auto result = std::string();

    for (auto& parameter: parameters)
    {
        if (!result.empty())
            result.push_back('&');

        result += urlEncode(parameter.name);
        result.push_back('=');
        result += urlEncode(parameter.value);
    }

    return result;
}

std::string URL::toString() const
{
    auto result = base;

    if (!parameters.empty())
        result += querySeparator(base) + query();

    if (!fragment.empty())
        result += "#" + fragment;

    return result;
}
} // namespace eacp::HTTP
