#pragma once

#import <Foundation/Foundation.h>

namespace eacp
{
template <typename T>
class CFRef
{
public:
    CFRef() = default;
    CFRef(T ref)
        : ref(ref)
    {
    }

    ~CFRef() { release(); }

    // Move-only: a second owning reference needs an explicit CFRetain.
    CFRef(const CFRef&) = delete;
    CFRef& operator=(const CFRef&) = delete;

    CFRef(CFRef&& other) noexcept
        : ref(other.ref)
    {
        other.ref = nullptr;
    }

    CFRef& operator=(CFRef&& other) noexcept
    {
        if (this != &other)
        {
            release();
            ref = other.ref;
            other.ref = nullptr;
        }

        return *this;
    }

    void release()
    {
        if (ref)
            CFRelease(ref);

        // Cleared so an explicit release() followed by the destructor is safe.
        ref = nullptr;
    }

    void reset(T other)
    {
        release();
        ref = other;
    }

    T get() const { return ref; }
    operator T() const { return ref; }
    explicit operator bool() const { return ref != nullptr; }

private:
    T ref = nullptr;
};

} // namespace eacp