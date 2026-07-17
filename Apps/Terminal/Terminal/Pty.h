#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <thread>

namespace term
{
struct PtySize
{
    int cols = 80;
    int rows = 24;
};

// A pseudo-terminal running the user's login shell. Output is delivered on a
// background reader thread via onOutput — marshal to the UI thread before
// touching any view state. onExit fires (also on the reader thread) when the
// shell terminates.
class Pty
{
public:
    Pty() = default;
    ~Pty();

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    bool start(const PtySize& size,
               std::function<void(std::string)> onOutput,
               std::function<void()> onExit);

    void write(std::string_view data);
    void resize(const PtySize& size);
    bool isRunning() const;
    void shutdown();

private:
    int fd = -1;
    long pid = -1;
    std::thread reader;
};
} // namespace term
