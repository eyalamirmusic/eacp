#pragma once

#include <cstring>

struct LayoutEntry
{
    char key[32];
    float x, y, width, height;
};

struct LayoutTable
{
    LayoutEntry* entries;
    int capacity;
    int count;
};

using LayoutFunction = void (*)(float width, float height, LayoutTable* table);

inline constexpr auto layoutFunctionName = "demoLayout";

inline void addLayoutEntry(
    LayoutTable* table, const char* key, float x, float y, float width, float height)
{
    if (table == nullptr || table->count >= table->capacity)
        return;

    auto& entry = table->entries[table->count++];
    auto keyLength = std::strlen(key);
    auto copied = keyLength < sizeof(entry.key) ? keyLength : sizeof(entry.key) - 1;

    std::memcpy(entry.key, key, copied);
    entry.key[copied] = '\0';
    entry.x = x;
    entry.y = y;
    entry.width = width;
    entry.height = height;
}
