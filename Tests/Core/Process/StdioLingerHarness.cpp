// Fixture for Process/inheritStdio/streamsWhileRunning: prints its first
// argument to the stdout it inherited, flushes, and stays alive until it is
// killed or the process whose id is its second argument is gone. Only the
// test decides when it ends, and a test that dies first cannot orphan it.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#ifdef _WIN32
#include <eacp/Core/Utils/WinInclude.h>
#else
#include <unistd.h>
#endif

namespace
{
void waitForParentToExit(long parent)
{
#ifdef _WIN32
    auto handle = OpenProcess(SYNCHRONIZE, FALSE, (DWORD) parent);

    if (handle == nullptr)
        return;

    WaitForSingleObject(handle, INFINITE);
    CloseHandle(handle);
#else
    while (getppid() == (pid_t) parent)
        std::this_thread::sleep_for(std::chrono::milliseconds {50});
#endif
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
        return 2;

    std::printf("%s\n", argv[1]);
    std::fflush(stdout);

    waitForParentToExit(std::stol(argv[2]));
    return 0;
}
