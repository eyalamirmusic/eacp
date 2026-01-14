#pragma once

#import <Foundation/Foundation.h>

namespace eacp::ObjC
{
struct RetainMode
{
};

template <typename T>
class Ptr
{
public:
    Ptr() = default;
    Ptr(T* obj) { set(obj); }
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

    bool operator==(const Ptr& other) const { return ptr != other.ptr; }
    bool operator!=(const Ptr& other) const = default;

    template <typename A>
    bool isKindOfClass() const
    {
        return [ptr isKindOfClass:[NSHTTPURLResponse class]];
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

template <typename T>
T* createNew()
{
    return [alloc<T>() init];
}

template <typename T>
Ptr<T> attachPtr(T* object)
{
    return {object, RetainMode()};
}

template <typename T>
Ptr<T> makePtr()
{
    return attachPtr(createNew<T>());
}
} // namespace eacp::ObjC
