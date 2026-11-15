#include "ThreadUtils.h"

#include <functional>
#include <thread>

namespace eacp::Threads
{
namespace
{
std::string_view threadNamePrefixOnCharBoundary(std::string_view text,
                                                std::size_t length)
{
    auto isContinuationByte = [&](std::size_t index)
    { return (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80; };

    while (length > 0 && length < text.size() && isContinuationByte(length))
        --length;

    return text.substr(0, length);
}

std::size_t threadNameNumberStart(std::string_view name)
{
    auto lastNonDigit = name.find_last_not_of("0123456789");
    auto start = lastNonDigit == std::string_view::npos ? 0 : lastNonDigit + 1;

    if (start == name.size())
        return start;

    if (start > 0 && name[start - 1] == ' ')
        --start;

    return start;
}
} // namespace

std::uint64_t currentThreadId()
{
    return static_cast<std::uint64_t>(
        std::hash<std::thread::id> {}(std::this_thread::get_id()));
}

RealtimeOptions RealtimeOptions::forAudio(int framesPerBuffer, double sampleRate)
{
    if (framesPerBuffer <= 0 || sampleRate <= 0.0)
        return {};

    auto options = RealtimeOptions {};
    options.maxProcessingTimeMs = framesPerBuffer / sampleRate * 1000.0;
    return options;
}

std::string fitThreadName(std::string_view name, int maxLength)
{
    if (maxLength <= 0)
        return {};

    auto limit = static_cast<std::size_t>(maxLength);

    if (name.size() <= limit)
        return std::string {name};

    auto suffix = name.substr(threadNameNumberStart(name));

    if (suffix.size() >= limit)
        return std::string {name.substr(name.size() - limit)};

    auto base = threadNamePrefixOnCharBoundary(name, limit - suffix.size());
    return std::string {base} + std::string {suffix};
}
} // namespace eacp::Threads
