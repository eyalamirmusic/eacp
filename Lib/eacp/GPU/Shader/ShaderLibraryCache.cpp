#include "ShaderLibraryCache.h"

#include "../Device/Device.h"
#include "Dxc.h"
#include "PrecompiledShaders.h"

#include <eacp/Core/Utils/Sha256.h>
#include <eacp/Core/Utils/Strings.h>

#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>

namespace eacp::GPU
{
namespace
{
using CacheKey = Sha256Digest;
using CachedLibrary = ShaderLibraryCache::LibraryPointer;

constexpr auto notYetFollowed = std::numeric_limits<std::uint64_t>::max();

struct CacheEntry
{
    CachedLibrary library;
    std::uint64_t backendGeneration = 0;
    bool ready = false;
};

using EntryPointer = std::shared_ptr<CacheEntry>;

void addKeyField(Sha256& hash, std::string_view field)
{
    hash.update(Strings::concat(field.size(), ":"));
    hash.update(field);
}

std::string dxcConfigurationOf(const ShaderSource& source)
{
    if (source.backend != ShaderBackend::DirectX || !Dxc::isEnabled())
        return "fxc";

    const auto status = Dxc::status();
    auto text =
        Strings::concat("dxc ", Dxc::stateName(status.state), " ", status.version);

    for (const auto& argument: Dxc::getOptions().arguments)
        text += " " + argument;

    return text;
}

CacheKey cacheKeyFor(const ShaderSource& source)
{
    const auto& group = source.threadGroup;
    auto hash = Sha256 {};

    addKeyField(hash, Strings::toString(static_cast<int>(source.backend)));
    addKeyField(hash, source.vertexEntry);
    addKeyField(hash, source.fragmentEntry);
    addKeyField(hash, source.computeEntry);
    addKeyField(hash, Strings::concat(group.x, ",", group.y, ",", group.z));
    addKeyField(hash, source.hasPipelineConstants ? "constants" : "none");
    addKeyField(hash, Strings::toString(Precompiled::configurationGeneration()));
    addKeyField(hash, dxcConfigurationOf(source));
    addKeyField(hash, source.source);

    return hash.finish();
}

void loadDxcOnTheOwningThread(const ShaderSource& source)
{
    if (source.backend == ShaderBackend::DirectX && Dxc::isEnabled())
        Dxc::load();
}

CachedLibrary compileLibrary(Device& device, const ShaderSource& source)
{
    return std::make_shared<const ShaderLibrary>(device, source);
}
} // namespace

struct ShaderLibraryCache::State
{
    bool isUsable(const CacheEntry& entry) const
    {
        return entry.ready && entry.library != nullptr && entry.library->isValid()
               && entry.backendGeneration == backendGeneration;
    }

    // The library leaves the entry here, under the lock, so a worker still
    // holding the entry can never be the one that releases it.
    static void evict(CacheEntry& entry, Vector<LibraryPointer>& evicted)
    {
        auto taken = LibraryPointer {};
        taken.swap(entry.library);
        evicted.add(taken);
    }

    void forgetOlderGenerations(std::uint64_t current,
                                Vector<LibraryPointer>& evicted)
    {
        if (current == backendGeneration)
            return;

        backendGeneration = current;

        for (auto it = entries.begin(); it != entries.end();)
        {
            if (it->second->ready)
            {
                evict(*it->second, evicted);
                it = entries.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    EntryPointer claim(const CacheKey& key)
    {
        auto entry = std::make_shared<CacheEntry>();
        entry->backendGeneration = backendGeneration;
        entries[key] = entry;
        ++counts.compiles;
        return entry;
    }

    void waitUntilReady(std::unique_lock<std::mutex>& lock,
                        const EntryPointer& entry)
    {
        auto isReady = [&entry] { return entry->ready; };
        finished.wait(lock, isReady);
    }

    void publish(const EntryPointer& entry, LibraryPointer& built)
    {
        entry->library.swap(built);
        entry->ready = true;
        finished.notify_all();
    }

    void abandon(const CacheKey& key, const EntryPointer& entry)
    {
        entry->ready = true;

        if (auto found = entries.find(key);
            found != entries.end() && found->second == entry)
            entries.erase(found);

        finished.notify_all();
    }

    EntryPointer find(const CacheKey& key) const
    {
        auto found = entries.find(key);
        return found != entries.end() ? found->second : nullptr;
    }

    mutable std::mutex mutex;
    std::condition_variable finished;
    std::map<CacheKey, EntryPointer> entries;
    std::uint64_t backendGeneration = notYetFollowed;
    Stats counts;
};

ShaderLibraryCache::ShaderLibraryCache() = default;

ShaderLibraryCache::~ShaderLibraryCache() = default;

ShaderLibraryCache& ShaderLibraryCache::of(Device& device)
{
    return device.perDevice<ShaderLibraryCache>();
}

ShaderLibraryCache::LibraryPointer
    ShaderLibraryCache::acquire(Device& device, const ShaderSource& source)
{
    loadDxcOnTheOwningThread(source);

    const auto key = cacheKeyFor(source);
    auto evicted = Vector<LibraryPointer> {};
    auto lock = std::unique_lock {state->mutex};

    state->forgetOlderGenerations(device.backendGeneration(), evicted);

    if (auto existing = state->find(key))
    {
        if (!existing->ready)
        {
            ++state->counts.waits;
            state->waitUntilReady(lock, existing);

            if (existing->library != nullptr
                && existing->backendGeneration == state->backendGeneration)
            {
                ++state->counts.hits;
                return existing->library;
            }
        }
        else if (state->isUsable(*existing))
        {
            ++state->counts.hits;
            return existing->library;
        }

        State::evict(*existing, evicted);
    }

    auto entry = state->claim(key);
    lock.unlock();

    auto built = LibraryPointer {};

    try
    {
        built = compileLibrary(device, source);
    }
    catch (...)
    {
        lock.lock();
        state->abandon(key, entry);
        throw;
    }

    auto result = built;

    lock.lock();
    state->publish(entry, built);
    return result;
}

ShaderLibraryCache::Warmed
    ShaderLibraryCache::warm(Device& device,
                             const ShaderSource& source,
                             const std::function<void()>& whileInFlight)
{
    const auto key = cacheKeyFor(source);
    auto lock = std::unique_lock {state->mutex};

    auto describe = [](const CacheEntry& entry, bool compiledNow)
    {
        auto warmed = Warmed {};
        warmed.compiledNow = compiledNow;

        if (entry.library == nullptr)
            return warmed;

        warmed.valid = entry.library->isValid();
        warmed.precompiled = entry.library->isPrecompiled();
        warmed.compilation = entry.library->compilationInfo();
        return warmed;
    };

    if (auto existing = state->find(key))
    {
        if (!existing->ready)
            state->waitUntilReady(lock, existing);

        if (existing->library != nullptr)
            return describe(*existing, false);
    }

    auto entry = state->claim(key);
    lock.unlock();

    auto built = LibraryPointer {};

    try
    {
        whileInFlight();
        built = compileLibrary(device, source);
    }
    catch (...)
    {
        lock.lock();
        state->abandon(key, entry);
        throw;
    }

    lock.lock();
    state->publish(entry, built);
    return describe(*entry, true);
}

void ShaderLibraryCache::followBackendGeneration(Device& device)
{
    auto evicted = Vector<LibraryPointer> {};
    auto lock = std::lock_guard {state->mutex};
    state->forgetOlderGenerations(device.backendGeneration(), evicted);
}

ShaderLibraryCache::LibraryPointer
    ShaderLibraryCache::find(const ShaderSource& source) const
{
    const auto key = cacheKeyFor(source);
    auto lock = std::lock_guard {state->mutex};
    auto entry = state->find(key);

    if (entry == nullptr || !state->isUsable(*entry))
        return nullptr;

    return entry->library;
}

bool ShaderLibraryCache::contains(const ShaderSource& source) const
{
    const auto key = cacheKeyFor(source);
    auto lock = std::lock_guard {state->mutex};
    auto entry = state->find(key);
    return entry != nullptr && entry->ready && entry->library != nullptr;
}

bool ShaderLibraryCache::isInFlight(const ShaderSource& source) const
{
    const auto key = cacheKeyFor(source);
    auto lock = std::lock_guard {state->mutex};
    auto entry = state->find(key);
    return entry != nullptr && !entry->ready;
}

ShaderLibraryCache::Stats ShaderLibraryCache::stats() const
{
    auto lock = std::lock_guard {state->mutex};
    auto result = state->counts;
    result.entries = static_cast<int>(state->entries.size());
    return result;
}
} // namespace eacp::GPU
