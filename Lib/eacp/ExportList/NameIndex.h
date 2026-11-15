#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace eacp::ExportList
{
// Names to values, by open addressing over plain arrays, with the names copied
// into large blocks. A closure defines hundreds of thousands of symbols, most
// of them repeats of a few tens of thousands of names; a node-based map, and a
// string allocation per name, make a Debug build of the tool, which is what a
// Debug build directory runs, several times slower than a Release one.
template <typename Value>
class NameIndex
{
public:
    struct Entry
    {
        std::string_view name;
        Value value;
    };

    // The value stored for name, or a new one made from initial. added says
    // which.
    Value& findOrAdd(std::string_view name, const Value& initial, bool& added)
    {
        if ((entries.size() + 1) * 2 > slots.size())
            grow();

        auto hash = std::hash<std::string_view> {}(name);
        auto mask = slots.size() - 1;

        for (auto slot = hash & mask;; slot = (slot + 1) & mask)
        {
            auto index = slots[slot];

            if (index == emptySlot)
            {
                slots[slot] = static_cast<std::uint32_t>(entries.size());
                hashes.push_back(hash);
                entries.push_back({store(name), initial});
                added = true;
                return entries.back().value;
            }

            if (hashes[index] == hash && entries[index].name == name)
            {
                added = false;
                return entries[index].value;
            }
        }
    }

    const std::vector<Entry>& all() const { return entries; }
    std::size_t size() const { return entries.size(); }

private:
    static constexpr auto emptySlot = ~std::uint32_t {0};
    static constexpr auto blockSize = std::size_t {1} << 20;

    std::string_view store(std::string_view name)
    {
        if (name.empty())
            return {};

        if (name.size() > blockCapacity - blockUsed)
        {
            blockCapacity = std::max(blockSize, name.size());
            blocks.push_back(std::make_unique<char[]>(blockCapacity));
            blockUsed = 0;
        }

        auto destination = blocks.back().get() + blockUsed;
        std::memcpy(destination, name.data(), name.size());

        blockUsed += name.size();
        return {destination, name.size()};
    }

    void grow()
    {
        auto size = std::max<std::size_t>(slots.size() * 2, 1024);
        slots.assign(size, emptySlot);
        auto mask = size - 1;

        for (auto index = std::size_t {0}; index < entries.size(); ++index)
        {
            auto slot = hashes[index] & mask;

            while (slots[slot] != emptySlot)
                slot = (slot + 1) & mask;

            slots[slot] = static_cast<std::uint32_t>(index);
        }
    }

    std::vector<std::uint32_t> slots;
    std::vector<std::size_t> hashes;
    std::vector<Entry> entries;
    std::vector<std::unique_ptr<char[]>> blocks;
    std::size_t blockCapacity = 0;
    std::size_t blockUsed = 0;
};
} // namespace eacp::ExportList
