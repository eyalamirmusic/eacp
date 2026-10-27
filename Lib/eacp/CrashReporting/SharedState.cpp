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

bool writeSharedEntry(SharedState& state,
                      SharedKeyValueTable& table,
                      std::string_view key,
                      std::string_view value)
{
    if (!fitsInField(key, maxMetadataKeyLength)
        || !fitsInField(value, maxMetadataValueLength))
        return false;

    for (auto index = 0u; index < table.count; ++index)
    {
        auto& entry = table.entries[index];

        if (readBoundedText(entry.key) == key)
        {
            auto write = SharedTableWrite {state};
            copyBoundedText(entry.value, value);
            return true;
        }
    }

    if (table.count >= maxMetadataEntries)
        return false;

    auto write = SharedTableWrite {state};
    auto& entry = table.entries[table.count];
    copyBoundedText(entry.key, key);
    copyBoundedText(entry.value, value);
    ++table.count;

    return true;
}

bool removeSharedEntry(SharedState& state,
                       SharedKeyValueTable& table,
                       std::string_view key)
{
    for (auto index = 0u; index < table.count; ++index)
    {
        if (readBoundedText(table.entries[index].key) != key)
            continue;

        auto write = SharedTableWrite {state};
        auto last = table.count - 1;
        table.entries[index] = table.entries[last];
        table.entries[last] = SharedMetadataEntry {};
        --table.count;

        return true;
    }

    return false;
}

bool writeSharedUser(SharedState& state, const User& user)
{
    if (!fitsInField(user.id, maxUserFieldLength)
        || !fitsInField(user.email, maxUserFieldLength)
        || !fitsInField(user.username, maxUserFieldLength))
        return false;

    auto write = SharedTableWrite {state};
    copyBoundedText(state.user.id, user.id);
    copyBoundedText(state.user.email, user.email);
    copyBoundedText(state.user.username, user.username);
    state.user.isSet = 1;

    return true;
}

void clearSharedUser(SharedState& state)
{
    auto write = SharedTableWrite {state};
    state.user = SharedUser {};
}

void addSharedBreadcrumb(SharedState& state,
                         const Breadcrumb& breadcrumb,
                         std::int64_t timestamp)
{
    auto write = SharedTableWrite {state};
    auto& ring = state.breadcrumbs;
    auto& entry = ring.entries[ring.added % maxBreadcrumbs];
    entry.timestamp = timestamp;
    copyBoundedText(entry.level, getLevelName(breadcrumb.level));
    copyBoundedText(entry.category, breadcrumb.category);
    copyBoundedText(entry.message, breadcrumb.message);
    ++ring.added;
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

std::map<std::string, std::string>
    readSharedEntries(const SharedState& state, const SharedKeyValueTable& table)
{
    return readSharedTableConsistently(
        state,
        [&]
        {
            auto entries = std::map<std::string, std::string> {};
            auto count = getBoundedCount(table.count, maxMetadataEntries);

            for (auto index = std::size_t {0}; index < count; ++index)
            {
                const auto& entry = table.entries[index];
                entries[std::string {readBoundedText(entry.key)}] =
                    std::string {readBoundedText(entry.value)};
            }

            return entries;
        });
}

User readSharedUser(const SharedState& state)
{
    return readSharedTableConsistently(
        state,
        [&]
        {
            if (state.user.isSet == 0)
                return User {};

            return User {std::string {readBoundedText(state.user.id)},
                         std::string {readBoundedText(state.user.email)},
                         std::string {readBoundedText(state.user.username)}};
        });
}

Vector<Breadcrumb> readSharedBreadcrumbs(const SharedState& state)
{
    return readSharedTableConsistently(
        state,
        [&]
        {
            const auto& ring = state.breadcrumbs;
            auto kept = std::min<std::uint64_t>(ring.added, maxBreadcrumbs);
            auto breadcrumbs = Vector<Breadcrumb> {};

            for (auto index = ring.added - kept; index < ring.added; ++index)
            {
                const auto& entry = ring.entries[index % maxBreadcrumbs];
                auto breadcrumb = Breadcrumb {};
                breadcrumb.category = std::string {readBoundedText(entry.category)};
                breadcrumb.message = std::string {readBoundedText(entry.message)};
                breadcrumb.level =
                    findLevel(readBoundedText(entry.level)).value_or(Level::info);
                breadcrumb.timestamp = entry.timestamp;
                breadcrumbs.add(breadcrumb);
            }

            return breadcrumbs;
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
