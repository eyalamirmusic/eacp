#pragma once

#include "View.h"
#include <eacp/Core/Utils/Containers.h>

namespace eacp::Graphics
{
template <typename T = View>
struct ViewList
{
    ViewList(View& parentToUse)
        : parent(parentToUse)
    {
    }

    using Container = OwnedVector<T>;

    auto begin() const { return views.begin(); }
    auto end() const { return views.end(); }

    Container* operator->() { return &views; }

    template <typename A = T, typename... Args>
    A& createVisible(Args&&... args)
    {
        auto& created = views.template createDerived<A>(std::forward<Args>(args)...);
        parent.addSubview(created);
        return created;
    }

    int size() const { return views.size(); }

    bool empty() const { return views.empty(); }

    bool erase(T& element)
    {
        auto idx = views.getIndexOfItem(&element);

        if (idx >= 0)
        {
            views.removeAt(idx);
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
