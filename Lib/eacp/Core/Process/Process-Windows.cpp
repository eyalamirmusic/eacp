#include "Process.h"
#include <algorithm>

#include "../Utils/WinInclude.h"

#include <cwctype>
#include <mutex>
#include <thread>
#include <vector>

namespace eacp::Processes
{
namespace
{
std::wstring toWide(const std::string& text)
{
    if (text.empty())
        return {};

    auto length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), (int) text.size(), nullptr, 0);

    auto result = std::wstring((std::size_t) length, L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text.data(), (int) text.size(), result.data(), length);
    return result;
}

// Quotes a single argument following the CommandLineToArgvW rules so that
// callers pass plain argument strings rather than a pre-escaped line.
std::wstring quoteArgument(const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;

    auto result = std::wstring {L"\""};

    for (auto it = arg.begin();; ++it)
    {
        auto backslashes = std::size_t {0};

        while (it != arg.end() && *it == L'\\')
        {
            ++it;
            ++backslashes;
        }

        if (it == arg.end())
        {
            result.append(backslashes * 2, L'\\');
            break;
        }
        else if (*it == L'"')
        {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
        }
        else
        {
            result.append(backslashes, L'\\');
            result.push_back(*it);
        }
    }

    result.push_back(L'"');
    return result;
}

std::wstring buildCommandLine(const std::string& executable,
                              const Vector<std::string>& arguments)
{
    auto line = quoteArgument(toWide(executable));

    for (const auto& arg: arguments)
    {
        line.push_back(L' ');
        line.append(quoteArgument(toWide(arg)));
    }

    return line;
}

std::wstring nameOf(const std::wstring& entry)
{
    auto equals = entry.find(L'=');
    return entry.substr(0, equals == std::wstring::npos ? entry.size() : equals);
}

std::wstring upper(std::wstring text)
{
    for (auto& c: text)
        c = (wchar_t) towupper((wint_t) c);
    return text;
}

// Windows needs the full environment block, so merge any overrides on top of
// the current process environment. Returns empty to mean "inherit unchanged".
std::wstring buildEnvironmentBlock(const Vector<EnvironmentVariable>& overrides)
{
    if (overrides.empty())
        return {};

    auto entries = Vector<std::wstring> {};

    if (auto existing = GetEnvironmentStringsW())
    {
        for (auto p = existing; *p;)
        {
            auto entry = std::wstring {p};
            entries.push_back(entry);
            p += entry.size() + 1;
        }

        FreeEnvironmentStringsW(existing);
    }

    for (const auto& var: overrides)
    {
        auto combined = toWide(var.name) + L"=" + toWide(var.value);
        auto key = upper(toWide(var.name));
        auto replaced = false;

        for (auto& entry: entries)
        {
            if (upper(nameOf(entry)) == key)
            {
                entry = combined;
                replaced = true;
                break;
            }
        }

        if (!replaced)
            entries.push_back(combined);
    }

    auto block = std::wstring {};

    for (const auto& entry: entries)
    {
        block.append(entry);
        block.push_back(L'\0');
    }

    block.push_back(L'\0');
    return block;
}
} // namespace

struct Process::Native
{
    explicit Native(ProcessOptions options)
        : detached(options.detached)
    {
        launch(std::move(options));
    }

    ~Native()
    {
        // A detached child is deliberately left running; just drop our handles.
        if (!detached && isRunning())
            ::TerminateProcess(processHandle, 1);

        closeInput();
        joinReaders();
        closeHandles();
    }

    bool launched() const { return processHandle != nullptr; }
    long id() const { return (long) processId; }

    bool isRunning() const
    {
        if (processHandle == nullptr)
            return false;

        return !reap(false);
    }

    int wait()
    {
        joinReaders();
        reap(true);
        return exitCode();
    }

    void terminate()
    {
        if (processHandle != nullptr)
            ::TerminateProcess(processHandle, 1);
    }

    void kill() { terminate(); }

    bool writeToInput(const std::string& data)
    {
        if (inputWrite == nullptr)
            return false;

        auto total = std::size_t {0};

        while (total < data.size())
        {
            auto chunk =
                (DWORD) std::min<std::size_t>(data.size() - total, 1u << 20);
            DWORD written = 0;

            if (!WriteFile(
                    inputWrite, data.data() + total, chunk, &written, nullptr))
                return false;

            total += written;
        }

        return true;
    }

    void closeInput()
    {
        if (inputWrite != nullptr)
        {
            CloseHandle(inputWrite);
            inputWrite = nullptr;
        }
    }

    std::string output() const
    {
        auto lock = std::lock_guard {bufferMutex};
        return outBuffer;
    }

    std::string errorOutput() const
    {
        auto lock = std::lock_guard {bufferMutex};
        return errBuffer;
    }

private:
    void launch(ProcessOptions options)
    {
        // Detached children get no pipes: nobody would drain them, and a
        // full pipe would eventually block the child.
        if (detached)
        {
            launchDetached(options);
            return;
        }

        SECURITY_ATTRIBUTES inherit {};
        inherit.nLength = sizeof inherit;
        inherit.bInheritHandle = TRUE;

        HANDLE inRead = nullptr, inWrite = nullptr;
        HANDLE outRead = nullptr, outWrite = nullptr;
        HANDLE errRead = nullptr, errWrite = nullptr;

        if (!CreatePipe(&inRead, &inWrite, &inherit, 0))
            return;

        if (!CreatePipe(&outRead, &outWrite, &inherit, 0))
        {
            CloseHandle(inRead);
            CloseHandle(inWrite);
            return;
        }

        if (!CreatePipe(&errRead, &errWrite, &inherit, 0))
        {
            CloseHandle(inRead);
            CloseHandle(inWrite);
            CloseHandle(outRead);
            CloseHandle(outWrite);
            return;
        }

        // The parent-side ends must not leak into the child.
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW startup {};
        startup.cb = sizeof startup;
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = inRead;
        startup.hStdOutput = outWrite;
        startup.hStdError = errWrite;

        auto commandLine = buildCommandLine(options.executable, options.arguments);
        commandLine.push_back(L'\0');

        auto workingDir = toWide(options.workingDirectory);
        auto environment = buildEnvironmentBlock(options.environment);

        PROCESS_INFORMATION info {};
        auto created = CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            environment.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT,
            environment.empty() ? nullptr : (LPVOID) environment.data(),
            workingDir.empty() ? nullptr : workingDir.c_str(),
            &startup,
            &info);

        CloseHandle(inRead);
        CloseHandle(outWrite);
        CloseHandle(errWrite);

        if (!created)
        {
            CloseHandle(inWrite);
            CloseHandle(outRead);
            CloseHandle(errRead);
            return;
        }

        processHandle = info.hProcess;
        threadHandle = info.hThread;
        processId = info.dwProcessId;
        inputWrite = inWrite;

        outReader = std::thread([this, outRead] { drain(outRead, outBuffer); });
        errReader = std::thread([this, errRead] { drain(errRead, errBuffer); });
    }

    void launchDetached(const ProcessOptions& options)
    {
        STARTUPINFOW startup {};
        startup.cb = sizeof startup;

        auto commandLine = buildCommandLine(options.executable, options.arguments);
        commandLine.push_back(L'\0');

        auto workingDir = toWide(options.workingDirectory);
        auto environment = buildEnvironmentBlock(options.environment);

        // DETACHED_PROCESS: no inherited console, so the child survives the
        // launcher's console/job the way a setsid'd POSIX child survives its
        // session.
        PROCESS_INFORMATION info {};
        auto created = CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            DETACHED_PROCESS
                | (environment.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT),
            environment.empty() ? nullptr : (LPVOID) environment.data(),
            workingDir.empty() ? nullptr : workingDir.c_str(),
            &startup,
            &info);

        if (!created)
            return;

        processHandle = info.hProcess;
        threadHandle = info.hThread;
        processId = info.dwProcessId;
    }

    void drain(HANDLE handle, std::string& dest)
    {
        char buffer[4096];
        DWORD count = 0;

        while (ReadFile(handle, buffer, sizeof buffer, &count, nullptr) && count > 0)
        {
            auto lock = std::lock_guard {bufferMutex};
            dest.append(buffer, count);
        }

        CloseHandle(handle);
    }

    void joinReaders()
    {
        if (outReader.joinable())
            outReader.join();

        if (errReader.joinable())
            errReader.join();
    }

    bool reap(bool blocking) const
    {
        auto lock = std::lock_guard {reapMutex};

        if (reaped)
            return true;

        if (processHandle == nullptr)
            return false;

        auto result = WaitForSingleObject(processHandle, blocking ? INFINITE : 0);

        if (result == WAIT_TIMEOUT)
            return false;

        DWORD code = 0;
        if (GetExitCodeProcess(processHandle, &code))
            exitStatus = code;

        reaped = true;
        return true;
    }

    int exitCode() const
    {
        auto lock = std::lock_guard {reapMutex};
        return reaped ? (int) exitStatus : -1;
    }

    void closeHandles()
    {
        if (threadHandle != nullptr)
        {
            CloseHandle(threadHandle);
            threadHandle = nullptr;
        }

        if (processHandle != nullptr)
        {
            CloseHandle(processHandle);
            processHandle = nullptr;
        }
    }

    HANDLE processHandle = nullptr;
    HANDLE threadHandle = nullptr;
    DWORD processId = 0;
    const bool detached;
    HANDLE inputWrite = nullptr;

    std::thread outReader;
    std::thread errReader;

    mutable std::mutex bufferMutex;
    std::string outBuffer;
    std::string errBuffer;

    mutable std::mutex reapMutex;
    mutable bool reaped = false;
    mutable DWORD exitStatus = 0;
};

Process::Process(ProcessOptions options)
    : impl(std::move(options))
{
}

Process::~Process() = default;

bool Process::launched() const
{
    return impl->launched();
}
bool Process::isRunning() const
{
    return impl->isRunning();
}
long Process::id() const
{
    return impl->id();
}

bool Process::writeToInput(const std::string& data)
{
    return impl->writeToInput(data);
}

void Process::closeInput()
{
    impl->closeInput();
}

int Process::wait()
{
    return impl->wait();
}
void Process::terminate()
{
    impl->terminate();
}
void Process::kill()
{
    impl->kill();
}

std::string Process::output() const
{
    return impl->output();
}
std::string Process::errorOutput() const
{
    return impl->errorOutput();
}
} // namespace eacp::Processes
