#pragma once

#import <Foundation/Foundation.h>

namespace eacp::ObjC
{
template <typename T>
class Ptr
{
public:
    Ptr(T* obj) { set(obj); }

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

    void setAndRetain(T* other)
    {
        if (ptr != other)
        {
            set(other);
            retain();
        }
    }

    Ptr(const Ptr& other) { setAndRetain(other.ptr); }

    Ptr& operator=(T* other)
    {
        set(other);
        return *this;
    }

    Ptr& operator=(const Ptr& other)
    {
        setAndRetain(other.ptr);
        return *this;
    }

    T* get() const { return ptr; }
    T* operator->() const { return ptr; }
    T* operator->() { return ptr; }

private:
    T* ptr {nullptr};
};

template <typename T>
T* alloc()
{ return [T alloc]; }

template <typename T>
T* createNew()
{ return [alloc<T>() init]; }

template <typename T>
Ptr<T> makePtr()
{ return {createNew<T>()}; }
} // namespace eacp::ObjC
