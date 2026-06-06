#include "Http.h"
#include "HttpProtocol.h"

#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Strings.h>

#include <eacp/Core/Utils/Containers.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

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

void appendFileFields(std::stringstream& body,
                      const std::string& boundary,
                      const Vector<FileField>& files)
{
    for (const auto& file: files)
    {
        auto stream = std::ifstream(file.filePath, std::ios::binary);
        auto content = std::string(std::istreambuf_iterator<char>(stream),
                                   std::istreambuf_iterator<char>());

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

Request& Request::addFileField(const std::string& fieldName,
                               const std::string& filePath,
                               const std::string& contentType)
{
    auto fileName = Files::filenameFromPath(filePath);
    fileFields.add({fieldName, filePath, contentType, fileName});
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
    worker.type = "GET";
    worker.progress = &progress;
    worker.headers["Range"] =
        "bytes=" + std::to_string(range.start) + "-" + std::to_string(range.end);

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
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
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

Response Request::downloadTo(const std::string& filePath) const
{
    if (parallelChunks > 1)
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
void appendDecodedPercentEscape(std::string& out,
                                const std::string& src,
                                std::size_t& i)
{
    auto hi = Strings::hexCharToInt(src[i + 1]);
    auto lo = Strings::hexCharToInt(src[i + 2]);

    if (hi < 0 || lo < 0)
    {
        out.push_back('%');
        return;
    }

    out.push_back((char) ((hi << 4) | lo));
    i += 2;
}
} // namespace

std::string urlDecode(const std::string& encoded)
{
    auto result = std::string();
    result.reserve(encoded.size());

    for (auto i = std::size_t {0}; i < encoded.size(); ++i)
    {
        auto c = encoded[i];

        if (c == '+')
            result.push_back(' ');
        else if (c == '%' && i + 2 < encoded.size())
            appendDecodedPercentEscape(result, encoded, i);
        else
            result.push_back(c);
    }

    return result;
}

namespace
{
void parseQueryPair(const std::string& pair, std::map<std::string, std::string>& out)
{
    if (pair.empty())
        return;

    auto eq = pair.find('=');
    if (eq == std::string::npos)
        out[urlDecode(pair)] = "";
    else
        out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
}
} // namespace

std::map<std::string, std::string> parseQueryString(const std::string& query)
{
    auto result = std::map<std::string, std::string>();
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
} // namespace eacp::HTTP
