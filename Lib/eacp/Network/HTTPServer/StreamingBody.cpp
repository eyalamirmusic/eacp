#include "HttpServerDispatcher.h"

#include "../HTTP/HttpProtocol.h"

#include <eacp/Core/Utils/File.h>

#include <charconv>

namespace eacp::HTTP
{

namespace
{

constexpr auto statusBadRequest = 400;
constexpr auto longestChunkSizeLine = std::size_t {8 * 1024};
constexpr auto chunkTerminator = std::string_view("\r\n");
constexpr auto continueInterimResponse = std::string_view("HTTP/1.1 100 Continue\r\n\r\n");
constexpr auto responseFileSliceSize = std::size_t {64 * 1024};
constexpr auto hexadecimalBase = 16;
constexpr auto decimalBase = 10;

std::optional<std::int64_t> parseWholeNumber(std::string_view text, int base)
{
    auto value = std::int64_t {0};
    auto* end = text.data() + text.size();
    auto [stoppedAt, error] = std::from_chars(text.data(), end, value, base);

    if (text.empty() || error != std::errc() || stoppedAt != end || value < 0)
        return std::nullopt;

    return value;
}

std::string_view trimSpaces(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);

    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);

    return text;
}

bool isChunkedTransferEncoding(const Request& head)
{
    auto encoding = Strings::toLower(head.getHeader("Transfer-Encoding"));
    return encoding.find("chunked") != std::string::npos;
}

bool expectsContinueResponse(const Request& head)
{
    return Strings::toLower(head.getHeader("Expect")) == "100-continue";
}

std::optional<std::int64_t> readDeclaredLength(const Request& head)
{
    auto contentLength = head.getHeader("Content-Length");

    if (contentLength.empty())
        return std::int64_t {0};

    auto length = parseWholeNumber(trimSpaces(contentLength), decimalBase);

    if (!length)
        throwError("Invalid Content-Length", statusBadRequest);

    return length;
}

std::size_t smallestOf(std::size_t a, std::size_t b, std::int64_t c)
{
    auto smaller = std::min(a, b);
    auto limit = static_cast<std::uint64_t>(c);

    if (limit < smaller)
        return static_cast<std::size_t>(limit);

    return smaller;
}

} // namespace

StreamingBodyDecoder::StreamingBodyDecoder(const Request& head, PeerStream peerToUse)
    : peer(std::move(peerToUse))
    , buffered(std::move(peer.alreadyReceived))
    , isChunked(isChunkedTransferEncoding(head))
    , expectsContinue(expectsContinueResponse(head))
{
    if (isChunked)
        return;

    declaredLength = readDeclaredLength(head);
    remainingInPiece = *declaredLength;
    isFinished = remainingInPiece == 0;
}

std::optional<std::int64_t> StreamingBodyDecoder::getDeclaredLength() const
{
    return declaredLength;
}

std::string StreamingBodyDecoder::readSome(std::size_t maxBytes)
{
    if (isFinished || maxBytes == 0)
        return {};

    sendContinueIfExpected();

    if (isChunked)
        return readSomeOfChunkedBody(maxBytes);

    receiveAtLeast(1);
    auto piece = takeBuffered(smallestOf(maxBytes, buffered.size(), remainingInPiece));
    remainingInPiece -= (std::int64_t) piece.size();
    isFinished = remainingInPiece == 0;
    return piece;
}

void StreamingBodyDecoder::sendContinueIfExpected()
{
    if (!expectsContinue)
        return;

    expectsContinue = false;
    peer.send(continueInterimResponse);
}

void StreamingBodyDecoder::receiveAtLeast(std::size_t byteCount)
{
    while (buffered.size() < byteCount)
    {
        auto more = peer.receiveMore();

        if (more.empty())
            throwError("The request body ended early", statusBadRequest);

        buffered.append(more);
    }
}

std::string StreamingBodyDecoder::takeBuffered(std::size_t byteCount)
{
    auto taken = buffered.substr(0, byteCount);
    buffered.erase(0, byteCount);
    return taken;
}

std::string StreamingBodyDecoder::readLine()
{
    while (true)
    {
        auto lineEnd = buffered.find(chunkTerminator);

        if (lineEnd != std::string::npos)
        {
            auto line = takeBuffered(lineEnd);
            buffered.erase(0, chunkTerminator.size());
            return line;
        }

        if (buffered.size() > longestChunkSizeLine)
            throwError("Chunk size line too long", statusBadRequest);

        receiveAtLeast(buffered.size() + 1);
    }
}

void StreamingBodyDecoder::beginNextChunk()
{
    auto wholeLine = readLine();
    auto line = std::string_view(wholeLine);
    auto extensionStart = line.find(';');

    if (extensionStart != std::string_view::npos)
        line = line.substr(0, extensionStart);

    auto chunkSize = parseWholeNumber(trimSpaces(line), hexadecimalBase);

    if (!chunkSize)
        throwError("Invalid chunk size", statusBadRequest);

    remainingInPiece = *chunkSize;

    if (remainingInPiece > 0)
        return;

    while (!readLine().empty())
    {
    }

    isFinished = true;
}

std::string StreamingBodyDecoder::readSomeOfChunkedBody(std::size_t maxBytes)
{
    if (remainingInPiece == 0)
        beginNextChunk();

    if (isFinished)
        return {};

    receiveAtLeast(1);
    auto piece = takeBuffered(smallestOf(maxBytes, buffered.size(), remainingInPiece));
    remainingInPiece -= (std::int64_t) piece.size();

    if (remainingInPiece > 0)
        return piece;

    receiveAtLeast(chunkTerminator.size());

    if (!buffered.starts_with(chunkTerminator))
        throwError("Chunk data not followed by CRLF", statusBadRequest);

    buffered.erase(0, chunkTerminator.size());
    return piece;
}

void sendResponse(const Response& response,
                  const std::function<void(std::string_view)>& send)
{
    if (response.contentFilePath.empty())
    {
        send(serializeResponse(response));
        return;
    }

    auto file = File(FilePath(response.contentFilePath));
    auto head = response;
    head.content.clear();
    head.setHeader("Content-Length", std::to_string(file.size()));
    send(serializeResponse(head));

    auto slice = std::vector<std::uint8_t>(responseFileSliceSize);
    auto offset = std::uint64_t {0};

    while (true)
    {
        auto readCount = file.read(offset, Span<std::uint8_t>(slice));

        if (readCount <= 0)
            return;

        auto* sliceStart = reinterpret_cast<const char*>(slice.data());
        send(std::string_view(sliceStart, (std::size_t) readCount));
        offset += (std::uint64_t) readCount;
    }
}

} // namespace eacp::HTTP
