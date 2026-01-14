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

    void release()
    {
        if (ref)
            CFRelease(ref);
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