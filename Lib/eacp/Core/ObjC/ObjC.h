#pragma once
/** @file
    ObjC::Ptr, a reference-counted owner for Objective-C objects, and allocation
    helpers. Apple only.
*/


#import <Foundation/Foundation.h>

namespace eacp::ObjC
{
/// Tag asking Ptr to retain the object it is given instead of adopting it.
struct RetainMode
{
};

/// Releases its object on destruction; copies retain.
template <typename T>
class Ptr
{
public:
    Ptr() = default;
    /// Adopts an owned (+1) object without retaining it, such as the result of
    /// alloc/init.
    Ptr(T* obj) { set(obj); }
    /// Retains the object, for one you do not own.
    Ptr(T* obj, RetainMode) { reset(obj); }
    Ptr(const Ptr& obj) { reset(obj.ptr); }

    ~Ptr() { release(); }

    void release()
    {
        if (ptr != nullptr)
            CFRelease((__bridge CFTypeRef) ptr);

        ptr = nullptr;
    }

    void set(T* object)
    {
        if (ptr != object)
        {
            release();
            ptr = object;
        }
    }

    void retain()
    {
        if (ptr != nullptr)
            CFRetain((__bridge CFTypeRef) ptr);
    }

    void reset(T* other)
    {
        if (ptr != other)
        {
            set(other);
            retain();
        }
    }

    /// Adopts without retaining, like the one-argument constructor.
    Ptr& operator=(T* other)
    {
        set(other);
        return *this;
    }

    Ptr& operator=(const Ptr& other)
    {
        reset(other.ptr);
        return *this;
    }

    operator bool() const { return ptr != nullptr; }

    bool operator==(T* other) const { return ptr == other; }
    bool operator!=(T* other) const { return !operator==(other); }

    bool operator==(const Ptr& other) const { return ptr == other.ptr; }
    bool operator!=(const Ptr& other) const { return !operator==(other); }

    template <typename A>
    bool isKindOfClass() const
    {
        return [ptr isKindOfClass:[A class]];
    }

    T* get() { return ptr; }
    const T* get() const { return ptr; }

    const T* operator->() const { return ptr; }
    T* operator->() { return ptr; }

private:
    T* ptr {nullptr};
};

template <typename T>
T* alloc()
{
    return [T alloc];
}

/// `[[T alloc] init]`; the caller owns the result.
template <typename T>
T* createNew()
{
    return [alloc<T>() init];
}

/// A Ptr that retains `object`, for one you do not own.
template <typename T>
Ptr<T> attachPtr(T* object)
{
    return {object, RetainMode()};
}

/// A Ptr owning a new `[[T alloc] init]`.
template <typename T>
Ptr<T> makePtr()
{
    return Ptr<T> {createNew<T>()};
}
} // namespace eacp::ObjC
