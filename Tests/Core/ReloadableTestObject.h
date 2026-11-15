#pragma once

struct ReloadableTestObject
{
    virtual ~ReloadableTestObject() = default;
    virtual int value() const = 0;

    void markDestroyed() const
    {
        if (destroyed != nullptr)
            *destroyed = true;
    }

    bool* destroyed = nullptr;
};
