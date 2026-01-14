#pragma once

#include <algorithm>

namespace eacp::Vectors
{

template<typename T, typename A>
auto find(const T& container, const A& element)
{
    return std::find(container.begin(), container.end(), element);
}

template <typename T, typename A>
bool contains(const T& container, const A& elementToCheck)
{
    return find(container, elementToCheck) != container.end();
}

template <typename T, typename A>
bool eraseMatch(T& container, const A& element)
{
    auto it = find(container, element);

    if (it != container.end())
    {
        container.erase(it);
        return true;
    }

    return false;
}
}
