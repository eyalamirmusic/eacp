#pragma once

#include <memory>

namespace eacp
{
template <typename T>
class Pimpl
{
public:
    template <typename... Args>
    Pimpl(Args&&... args)
    {
        ptr = std::make_shared<T>(std::forward<Args>(args)...);
    }

    Pimpl(const Pimpl& other) = delete;
    Pimpl(Pimpl&& other) noexcept = default;

    T* get() { return ptr.get(); }
    const T* get() const { return ptr.get(); }

    T* operator->() { return ptr.get(); }
    const T* operator->() const { return ptr.get(); }

    T& operator*() { return *ptr; }
    const T& operator*() const { return *ptr; }

private:
    std::shared_ptr<T> ptr;
};
} // namespace eacp