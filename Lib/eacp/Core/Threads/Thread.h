#pragma once

#include "../Utils/Common.h"

namespace eacp::Threads
{
class Thread
{
public:
    Thread();
    explicit Thread(const Callback& body);
    ~Thread();

    Thread(Thread&& other) noexcept;
    Thread& operator=(Thread&& other) noexcept;

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    void start(const Callback& body);
    void join();

    [[nodiscard]] bool isJoinable() const;
    [[nodiscard]] bool isCurrentThread() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Threads
