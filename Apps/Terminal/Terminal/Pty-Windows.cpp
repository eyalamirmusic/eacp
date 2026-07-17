#include "Pty.h"

#include <eacp/Core/Utils/WinInclude.h>
#include <eacp/Graphics/Helpers/StringUtils-Windows.h>

#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <cctype>
#include <vector>

namespace term
{
namespace
{
using eacp::Graphics::fromWideString;
using eacp::Graphics::toWideString;

std::wstring envVariable(const wchar_t* name)
{
    wchar_t buffer[MAX_PATH] = {};
    const auto length = GetEnvironmentVariableW(name, buffer, MAX_PATH);

    if (length == 0 || length >= MAX_PATH)
        return {};

    return {buffer, length};
}

std::wstring findShell()
{
    wchar_t path[MAX_PATH] = {};

    for (const auto* candidate: {L"pwsh.exe", L"powershell.exe"})
        if (SearchPathW(nullptr, candidate, nullptr, MAX_PATH, path, nullptr) > 0)
            return path;

    if (auto comspec = envVariable(L"ComSpec"); !comspec.empty())
        return comspec;

    return L"cmd.exe";
}

bool isDirectory(const std::wstring& path)
{
    const auto attributes = GetFileAttributesW(path.c_str());

    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring startDirectory(const std::string& requested)
{
    if (auto wide = toWideString(requested); !wide.empty() && isDirectory(wide))
        return wide;

    return envVariable(L"USERPROFILE");
}

COORD toCoord(const PtySize& size)
{
    return {(SHORT) size.cols, (SHORT) size.rows};
}

ULONGLONG creationTime(DWORD pid)
{
    auto* handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

    if (handle == nullptr)
        return 0;

    FILETIME created {}, exited {}, kernel {}, user {};
    auto time = ULONGLONG {0};

    if (GetProcessTimes(handle, &created, &exited, &kernel, &user))
        time = ((ULONGLONG) created.dwHighDateTime << 32) | created.dwLowDateTime;

    CloseHandle(handle);
    return time;
}

std::string baseName(const std::wstring& exeFile)
{
    auto name = fromWideString(exeFile);

    if (name.size() > 4)
    {
        auto extension = name.substr(name.size() - 4);
        std::transform(extension.begin(),
                       extension.end(),
                       extension.begin(),
                       [](unsigned char c) { return (char) std::tolower(c); });

        if (extension == ".exe")
            name.resize(name.size() - 4);
    }

    return name;
}
} // namespace

Pty::~Pty()
{
    shutdown();
}

bool Pty::start(const PtyOptions& options,
                std::function<void(std::string)> onOutput,
                std::function<void()> onExit)
{
    if (process != nullptr)
        return false;

    auto inputRead = HANDLE {};
    auto outputWrite = HANDLE {};
    auto inWrite = HANDLE {};
    auto outRead = HANDLE {};

    if (!CreatePipe(&inputRead, &inWrite, nullptr, 0))
        return false;

    if (!CreatePipe(&outRead, &outputWrite, nullptr, 0))
    {
        CloseHandle(inputRead);
        CloseHandle(inWrite);
        return false;
    }

    auto pseudoConsole = HPCON {};

    if (FAILED(CreatePseudoConsole(
            toCoord(options.size), inputRead, outputWrite, 0, &pseudoConsole)))
    {
        for (auto* handle: {inputRead, inWrite, outRead, outputWrite})
            CloseHandle(handle);

        return false;
    }

    auto listSize = SIZE_T {0};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &listSize);

    auto listStorage = std::vector<std::byte>(listSize);
    auto* attributes = (LPPROC_THREAD_ATTRIBUTE_LIST) listStorage.data();

    auto startup = STARTUPINFOEXW {};
    startup.StartupInfo.cb = sizeof startup;
    startup.lpAttributeList = attributes;

    auto info = PROCESS_INFORMATION {};
    const auto shell = findShell();
    auto commandLine = L"\"" + shell + L"\"";
    const auto directory = startDirectory(options.workingDirectory);

    const auto launched =
        InitializeProcThreadAttributeList(attributes, 1, 0, &listSize)
        && UpdateProcThreadAttribute(attributes,
                                     0,
                                     PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                     pseudoConsole,
                                     sizeof pseudoConsole,
                                     nullptr,
                                     nullptr)
        && CreateProcessW(shell.c_str(),
                          commandLine.data(),
                          nullptr,
                          nullptr,
                          FALSE,
                          EXTENDED_STARTUPINFO_PRESENT,
                          nullptr,
                          directory.empty() ? nullptr : directory.c_str(),
                          &startup.StartupInfo,
                          &info);

    DeleteProcThreadAttributeList(attributes);

    // The pseudoconsole duplicated its ends of both pipes; ours can go.
    CloseHandle(inputRead);
    CloseHandle(outputWrite);

    if (!launched)
    {
        ClosePseudoConsole(pseudoConsole);
        CloseHandle(inWrite);
        CloseHandle(outRead);
        return false;
    }

    CloseHandle(info.hThread);

    console = pseudoConsole;
    process = info.hProcess;
    inputWrite = inWrite;
    outputRead = outRead;

    reader = std::thread(
        [readHandle = outRead,
         output = std::move(onOutput),
         exit = std::move(onExit)]
        {
            char buffer[65536];

            while (true)
            {
                auto count = DWORD {0};

                if (!ReadFile(readHandle, buffer, sizeof buffer, &count, nullptr)
                    || count == 0)
                    break;

                output(std::string {buffer, count});
            }

            exit();
        });

    // ConPTY keeps the output pipe open until the pseudoconsole is closed, so
    // the reader would never see the shell exit on its own. Waiting on the
    // process and closing the console is what breaks the pipe and lets the
    // reader finish (delivering onExit).
    waiter = std::thread(
        [this, processHandle = info.hProcess]
        {
            WaitForSingleObject(processHandle, INFINITE);
            closeConsole();
        });

    return true;
}

void Pty::write(std::string_view data)
{
    auto remaining = data;

    while (inputWrite != nullptr && !remaining.empty())
    {
        auto written = DWORD {0};

        if (!WriteFile(inputWrite,
                       remaining.data(),
                       (DWORD) remaining.size(),
                       &written,
                       nullptr))
            break;

        remaining.remove_prefix(written);
    }
}

void Pty::resize(const PtySize& size)
{
    auto lock = std::lock_guard {consoleLock};

    if (console != nullptr)
        ResizePseudoConsole((HPCON) console, toCoord(size));
}

namespace
{
struct TreeInfo
{
    DWORD youngestPid = 0;
    std::wstring youngestName;
    std::wstring rootName;
};

// What the user sees running is the youngest process in the shell's
// descendant tree; the shell itself is the fallback.
TreeInfo inspectProcessTree(DWORD rootPid)
{
    auto info = TreeInfo {};
    auto* snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (snapshot == INVALID_HANDLE_VALUE)
        return info;

    struct Candidate
    {
        DWORD pid;
        DWORD parent;
        std::wstring name;
    };

    auto candidates = std::vector<Candidate> {};

    auto entry = PROCESSENTRY32W {};
    entry.dwSize = sizeof entry;

    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (entry.th32ProcessID == rootPid)
                info.rootName = entry.szExeFile;
            else
                candidates.push_back({entry.th32ProcessID,
                                      entry.th32ParentProcessID,
                                      entry.szExeFile});
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);

    auto tree = std::vector<DWORD> {rootPid};
    auto bestTime = ULONGLONG {0};

    for (auto index = std::size_t {0}; index < tree.size(); ++index)
    {
        for (const auto& candidate: candidates)
        {
            if (candidate.parent != tree[index]
                || std::find(tree.begin(), tree.end(), candidate.pid) != tree.end())
                continue;

            tree.push_back(candidate.pid);

            if (const auto time = creationTime(candidate.pid); time >= bestTime)
            {
                bestTime = time;
                info.youngestPid = candidate.pid;
                info.youngestName = candidate.name;
            }
        }
    }

    return info;
}

// The live working directory of another process, read from its PEB. The
// layout matches 64-bit Windows (x64 and ARM64): RTL_USER_PROCESS_PARAMETERS
// keeps CurrentDirectory right after the standard handles.
std::string processWorkingDirectory(DWORD pid)
{
    using NtQueryInformationProcessFn =
        NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

    static const auto query = (NtQueryInformationProcessFn) (void*) GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");

    if (query == nullptr)
        return {};

    auto* handle =
        OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);

    if (handle == nullptr)
        return {};

    auto result = std::string {};
    auto info = PROCESS_BASIC_INFORMATION {};
    auto peb = PEB {};

    struct Curdir
    {
        UNICODE_STRING DosPath;
        HANDLE Handle;
    };

    struct ParametersPrefix
    {
        ULONG MaximumLength;
        ULONG Length;
        ULONG Flags;
        ULONG DebugFlags;
        HANDLE ConsoleHandle;
        ULONG ConsoleFlags;
        HANDLE StandardInput;
        HANDLE StandardOutput;
        HANDLE StandardError;
        Curdir CurrentDirectory;
    };

    auto parameters = ParametersPrefix {};

    const auto read = [&](const void* address, void* into, std::size_t bytes)
    {
        auto copied = SIZE_T {0};
        return ReadProcessMemory(handle, address, into, bytes, &copied) != 0
               && copied == bytes;
    };

    if (query(handle, ProcessBasicInformation, &info, sizeof info, nullptr) == 0
        && read(info.PebBaseAddress, &peb, sizeof peb)
        && read(peb.ProcessParameters, &parameters, sizeof parameters)
        && parameters.CurrentDirectory.DosPath.Buffer != nullptr
        && parameters.CurrentDirectory.DosPath.Length > 0)
    {
        auto path = std::wstring(
            parameters.CurrentDirectory.DosPath.Length / sizeof(wchar_t), L'\0');

        if (read(parameters.CurrentDirectory.DosPath.Buffer,
                 path.data(),
                 parameters.CurrentDirectory.DosPath.Length))
        {
            while (path.size() > 3 && path.back() == L'\\')
                path.pop_back();

            result = fromWideString(path);
        }
    }

    CloseHandle(handle);
    return result;
}
} // namespace

std::string Pty::foregroundProcess() const
{
    if (process == nullptr)
        return {};

    const auto info = inspectProcessTree(GetProcessId(process));
    return baseName(info.youngestName.empty() ? info.rootName : info.youngestName);
}

std::string Pty::currentWorkingDirectory() const
{
    if (process == nullptr)
        return {};

    const auto rootPid = GetProcessId(process);
    const auto info = inspectProcessTree(rootPid);

    // The youngest descendant has the truthful cwd (PowerShell keeps its
    // process directory pinned at its start dir); the shell is the fallback.
    if (info.youngestPid != 0)
        if (auto dir = processWorkingDirectory(info.youngestPid); !dir.empty())
            return dir;

    return processWorkingDirectory(rootPid);
}

bool Pty::isRunning() const
{
    return process != nullptr && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

void Pty::closeConsole()
{
    auto lock = std::lock_guard {consoleLock};

    if (console != nullptr)
    {
        ClosePseudoConsole((HPCON) console);
        console = nullptr;
    }
}

void Pty::shutdown()
{
    // Closing the pseudoconsole ends the console session, which terminates
    // the attached shell the way closing a console window would.
    closeConsole();

    if (process != nullptr && WaitForSingleObject(process, 2000) == WAIT_TIMEOUT)
        TerminateProcess(process, 1);

    if (waiter.joinable())
        waiter.join();

    if (reader.joinable())
        reader.join();

    for (auto* handle: {&process, &inputWrite, &outputRead})
    {
        if (*handle != nullptr)
        {
            CloseHandle(*handle);
            *handle = nullptr;
        }
    }
}
} // namespace term
