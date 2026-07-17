#include "MruStore.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace term
{
MruStore::MruStore(const emberstore::Database& db)
    : stamps(db.collection<Stamp>("mru"))
{
    prune();
}

void MruStore::touch(const std::string& key, std::int64_t atMs)
{
    stamps.doc(key).set({atMs});
}

std::int64_t MruStore::lastUsed(const std::string& key)
{
    if (auto stamp = stamps.get(key))
        return stamp->at;

    return 0;
}

void MruStore::prune()
{
    if (stamps.size() <= maxEntries)
        return;

    auto aged = std::vector<std::pair<std::int64_t, std::string>> {};

    for (auto& id: stamps.ids())
        aged.emplace_back(lastUsed(id), id);

    std::sort(aged.begin(), aged.end());

    auto excess = aged.size() - maxEntries;

    for (std::size_t i = 0; i < excess; ++i)
        stamps.remove(aged[i].second);
}

std::int64_t MruStore::nowMs()
{
    using namespace std::chrono;
    return (std::int64_t) duration_cast<milliseconds>(
               system_clock::now().time_since_epoch())
        .count();
}
} // namespace term
