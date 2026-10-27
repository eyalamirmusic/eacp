#include "SharedState.h"

#include <algorithm>
#include <cstring>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto sharedStateReadAttempts = 8;

struct SharedTableWrite
{
    explicit SharedTableWrite(SharedState& stateToUse)
        : state(stateToUse)
    {
        state.tableSequence.fetch_add(1, std::memory_order_acq_rel);
    }

    ~SharedTableWrite()
    {
        state.tableSequence.fetch_add(1, std::memory_order_release);
    }

    SharedState& state;
};

bool isWriteInProgress(std::uint32_t sequence)
{
    return (sequence & 1u) != 0;
}

template <typename ReadFunction>
auto readSharedTableConsistently(const SharedState& state, ReadFunction read)
{
    auto result = read();

    for (auto attempt = 0; attempt < sharedStateReadAttempts; ++attempt)
    {
        auto before = state.tableSequence.load(std::memory_order_acquire);
        result = read();
        auto after = state.tableSequence.load(std::memory_order_acquire);

        if (before == after && !isWriteInProgress(before))
            break;
    }

    return result;
}

bool fitsInField(std::string_view text, std::size_t capacity)
{
    return text.size() < capacity;
}

std::size_t getBoundedCount(std::uint32_t count, std::size_t capacity)
{
    return std::min(static_cast<std::size_t>(count), capacity);
}
} // namespace

void copyBoundedText(char* destination, std::size_t capacity, std::string_view text)
{
    auto length = std::min(text.size(), capacity - 1);
    std::memcpy(destination, text.data(), length);
    destination[length] = '\0';
}

std::string_view readBoundedText(const char* source, std::size_t capacity)
{
    return {source, strnlen(source, capacity)};
}

void initialiseSharedState(SharedState& state, std::uint64_t appProcessId)
{
    std::memset(static_cast<void*>(&state), 0, sizeof(SharedState));
    state.magic = sharedStateMagic;
    state.version = sharedStateVersion;
    state.appProcessId = appProcessId;
}

bool isSharedStateValid(const SharedState& state)
{
    return state.magic == sharedStateMagic && state.version == sharedStateVersion;
}

std::optional<std::size_t> findSharedModuleIndex(const SharedCrashInfo& crash,
                                                 std::uint64_t address)
{
    auto count = getBoundedCount(crash.moduleCount, maxSharedModules);

    for (auto index = std::size_t {0}; index < count; ++index)
    {
        const auto& module = crash.modules[index];

        if (address >= module.base && address < module.base + module.size)
            return index;
    }

    return std::nullopt;
}

bool writeSharedMetadata(SharedState& state,
                         std::string_view key,
                         std::string_view value)
{
    if (!fitsInField(key, maxMetadataKeyLength)
        || !fitsInField(value, maxMetadataValueLength))
        return false;

    for (auto index = 0u; index < state.metadataCount; ++index)
    {
        auto& entry = state.metadata[index];

        if (readBoundedText(entry.key) == key)
        {
            auto write = SharedTableWrite {state};
            copyBoundedText(entry.value, value);
            return true;
        }
    }

    if (state.metadataCount >= maxMetadataEntries)
        return false;

    auto write = SharedTableWrite {state};
    auto& entry = state.metadata[state.metadataCount];
    copyBoundedText(entry.key, key);
    copyBoundedText(entry.value, value);
    ++state.metadataCount;

    return true;
}

bool writeSharedAttachment(SharedState& state, const FilePath& path)
{
    if (!fitsInField(path.str(), maxAttachmentPathLength)
        || state.attachmentCount >= maxAttachments)
        return false;

    auto write = SharedTableWrite {state};
    copyBoundedText(state.attachments[state.attachmentCount], path.str());
    ++state.attachmentCount;

    return true;
}

std::map<std::string, std::string> readSharedMetadata(const SharedState& state)
{
    return readSharedTableConsistently(
        state,
        [&]
        {
            auto metadata = std::map<std::string, std::string> {};
            auto count = getBoundedCount(state.metadataCount, maxMetadataEntries);

            for (auto index = std::size_t {0}; index < count; ++index)
            {
                const auto& entry = state.metadata[index];
                metadata[std::string {readBoundedText(entry.key)}] =
                    std::string {readBoundedText(entry.value)};
            }

            return metadata;
        });
}

Vector<FilePath> readSharedAttachments(const SharedState& state)
{
    return readSharedTableConsistently(
        state,
        [&]
        {
            auto attachments = Vector<FilePath> {};
            auto count = getBoundedCount(state.attachmentCount, maxAttachments);

            for (auto index = std::size_t {0}; index < count; ++index)
                attachments.add(
                    FilePath {readBoundedText(state.attachments[index])});

            return attachments;
        });
}
} // namespace eacp::CrashReporting::Detail
