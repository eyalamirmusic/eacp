#pragma once

#include "View.h"

namespace eacp::Graphics
{
template <typename T = View>
struct ViewList
{
    ViewList(View& parentToUse)
        : parent(parentToUse)
    {
    }

    using Container = std::vector<std::unique_ptr<T>>;

    auto begin() const { return views.begin(); }
    auto end() const { return views.end(); }

    Container* operator->() { return &views; }

    template <typename A = T, typename... Args>
    A& createVisible(Args&&... args)
    {
        auto newChild = new A(std::forward<Args>(args)...);
        parent.addSubview(*newChild);
        views.emplace_back(newChild);
        return *newChild;
    }

    int size() const { return (int) views.size(); }

    bool empty() const { return views.empty(); }

    bool erase(T& element)
    {
        auto it = std::ranges::find_if(
            views, [&](const auto& c) { return c.get() == &element; });

        if (it != end())
        {
            views.erase(it);
            return true;
        }

        return false;
    }

    T& front() { return *views.front(); }
    T& back() { return *views.back(); }

    View& parent;
    Container views;
};

} // namespace eacp::Graphics