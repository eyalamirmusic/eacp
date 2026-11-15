#include <eacp/Core/Core.h>
#include <eacp/Core/Utils/WinInclude.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <process.h>
#include <string>
#include <vector>

#include <tlhelp32.h>

// eacp's loop in a process of its own, for EventLoopTests-Windows.cpp, which
// reads what each mode prints. Only the first pass of the loop in a process
// installs the parent-death watchdog, and only a child has a parent the test
// controls.
//
// "run" (through Apps::runConsole) and "runFor" (one EventLoop::runFor pass,
// as a test runner flushing the loop does) print how many threads the C
// runtime started while the loop ran are still alive afterwards: the loop
// starts none. A program built with the static runtime could otherwise leave
// one still starting as it exits, entering the runtime after exit() has shut
// it down. Threads other libraries start for themselves are not counted.
//
// "wait" runs the loop until something quits it, then exits with
// quitExitCode. "parent" starts a "wait" child, prints its id once its loop is
// running, and exits when its own input closes: the parent whose exit the
// child's loop must notice.
//
// "detach" prints the process's handle count while the watchdog is installed,
// after detachCurrentThreadAsMain and after a later attach and loop pass.
namespace
{
using NtQueryInformationThreadFunc = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);

constexpr auto threadQuerySetWin32StartAddress = 9;
constexpr auto quitExitCode = 7;
constexpr auto loopRunningLine = "loop running";

std::vector<DWORD> threadsOfThisProcess()
{
    auto threads = std::vector<DWORD> {};
    auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);

    if (snapshot == INVALID_HANDLE_VALUE)
        return threads;

    auto entry = THREADENTRY32 {};
    entry.dwSize = sizeof(entry);
    auto self = GetCurrentProcessId();

    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID == self)
                threads.push_back(entry.th32ThreadID);
        } while (Thread32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return threads;
}

HMODULE moduleAt(const void* address)
{
    auto module = HMODULE {};

    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(address),
                       &module);

    return module;
}

HMODULE startModuleOf(DWORD threadId)
{
    auto query = reinterpret_cast<NtQueryInformationThreadFunc>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    auto thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, threadId);
    void* start = nullptr;

    if (thread != nullptr && query != nullptr)
        query(
            thread, threadQuerySetWin32StartAddress, &start, sizeof(start), nullptr);

    if (thread != nullptr)
        CloseHandle(thread);

    return start != nullptr ? moduleAt(start) : nullptr;
}

int countNewRuntimeThreads(const std::vector<DWORD>& before)
{
    auto runtime = moduleAt(reinterpret_cast<const void*>(&_beginthreadex));
    auto after = threadsOfThisProcess();

    return (int) std::count_if(after.begin(),
                               after.end(),
                               [&](DWORD id)
                               {
                                   return std::find(before.begin(), before.end(), id)
                                              == before.end()
                                          && startModuleOf(id) == runtime;
                               });
}

int reportRuntimeThreadsLeft(bool throughRunFor, int argc, char* argv[])
{
    auto before = threadsOfThisProcess();

    if (throughRunFor)
        eacp::Threads::getEventLoop().runFor(eacp::Time::MS {0});
    else
        eacp::Apps::runConsole(argc, argv, [] { return 0; });

    std::printf("runtime threads left: %d\n", countNewRuntimeThreads(before));
    return 0;
}

int runUntilQuit()
{
    eacp::Threads::callAsync(
        []
        {
            std::printf("%s\n", loopRunningLine);
            std::fflush(stdout);
        });

    eacp::Threads::getEventLoop().run();
    return quitExitCode;
}

std::wstring commandLineFor(const char* mode)
{
    auto path = std::wstring(MAX_PATH, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), MAX_PATH));

    return L"\"" + path + L"\" " + std::wstring(mode, mode + std::strlen(mode));
}

bool readUntilLoopRuns(HANDLE output)
{
    auto text = std::string {};
    char buffer[256];
    auto read = DWORD {};

    while (text.find(loopRunningLine) == std::string::npos)
    {
        if (!ReadFile(output, buffer, sizeof(buffer), &read, nullptr) || read == 0)
            return false;

        text.append(buffer, read);
    }

    return true;
}

void waitForInputToClose()
{
    while (std::fgetc(stdin) != EOF)
    {
    }
}

int startAChildThenExit()
{
    auto security = SECURITY_ATTRIBUTES {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;

    if (!CreatePipe(&readEnd, &writeEnd, &security, 0))
        return 1;

    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    // Only the pipe is inherited. The test's own pipes to this process are
    // inheritable too, and a child holding one would keep the test reading
    // this process's output until the child exits.
    auto attributesSize = SIZE_T {};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributesSize);
    auto attributes = std::vector<char>(attributesSize);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());

    if (!InitializeProcThreadAttributeList(list, 1, 0, &attributesSize)
        || !UpdateProcThreadAttribute(list,
                                      0,
                                      PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      &writeEnd,
                                      sizeof(writeEnd),
                                      nullptr,
                                      nullptr))
        return 1;

    auto startup = STARTUPINFOEXW {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = writeEnd;
    startup.StartupInfo.hStdError = writeEnd;
    startup.lpAttributeList = list;

    auto info = PROCESS_INFORMATION {};
    auto commandLine = commandLineFor("wait");

    auto started = CreateProcessW(nullptr,
                                  commandLine.data(),
                                  nullptr,
                                  nullptr,
                                  TRUE,
                                  EXTENDED_STARTUPINFO_PRESENT,
                                  nullptr,
                                  nullptr,
                                  &startup.StartupInfo,
                                  &info);
    DeleteProcThreadAttributeList(list);

    if (!started)
        return 1;

    CloseHandle(writeEnd);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);

    if (!readUntilLoopRuns(readEnd))
        return 1;

    std::printf("child %lu\n", info.dwProcessId);
    std::fflush(stdout);

    waitForInputToClose();
    return 0;
}

int handleCount()
{
    auto count = DWORD {};
    GetProcessHandleCount(GetCurrentProcess(), &count);
    return (int) count;
}

int reportHandlesAcrossDetach()
{
    auto passOfTheLoop = []
    { eacp::Threads::getEventLoop().runFor(eacp::Time::MS {0}); };

    passOfTheLoop();
    auto watching = handleCount();

    eacp::Threads::detachCurrentThreadAsMain();
    auto detached = handleCount();

    eacp::Threads::attachCurrentThreadAsMain();
    passOfTheLoop();
    auto again = handleCount();

    std::printf(
        "handles watching %d detached %d again %d\n", watching, detached, again);
    return 0;
}

bool isMode(int argc, char* argv[], const char* mode)
{
    return argc > 1 && std::strcmp(argv[1], mode) == 0;
}
} // namespace

int main(int argc, char* argv[])
{
    if (isMode(argc, argv, "wait"))
        return runUntilQuit();

    if (isMode(argc, argv, "parent"))
        return startAChildThenExit();

    if (isMode(argc, argv, "detach"))
        return reportHandlesAcrossDetach();

    return reportRuntimeThreadsLeft(isMode(argc, argv, "runFor"), argc, argv);
}
