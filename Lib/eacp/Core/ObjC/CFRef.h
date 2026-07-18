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

    // Move-only, and deliberately so. The implicitly generated copy duplicated
    // the pointer without a matching CFRetain, so both copies released it — a
    // double free surfacing as a crash inside CFRelease, a long way from the
    // copy that caused it. Anything that wants a second owning reference has to
    // say so with an explicit CFRetain.
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

        // Clearing matters now that release() is reachable twice: once
        // explicitly and again from the destructor.
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