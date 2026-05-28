#include "Process.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace eacp::WebView::Test
{

namespace
{

struct OutputState
{
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    std::string outBuf;
    std::string errBuf;
    bool outEof = false;
    bool errEof = false;

    void appendOut(std::string_view bytes)
    {
        auto lock = std::lock_guard {mutex};
        outBuf.append(bytes);
        cv.notify_all();
    }

    void appendErr(std::string_view bytes)
    {
        auto lock = std::lock_guard {mutex};
        errBuf.append(bytes);
        cv.notify_all();
    }

    void markOutEof()
    {
        auto lock = std::lock_guard {mutex};
        outEof = true;
        cv.notify_all();
    }

    void markErrEof()
    {
        auto lock = std::lock_guard {mutex};
        errEof = true;
        cv.notify_all();
    }
};

std::wstring utf8ToWide(std::string_view utf8)
{
    if (utf8.empty())
        return {};
    auto needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                        static_cast<int>(utf8.size()),
                                        nullptr, 0);
    auto out = std::wstring(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                          static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

// Quote one arg per Microsoft's CommandLineToArgvW rules. Reference:
// https://learn.microsoft.com/en-us/archive/blogs/twistylittlepassagesallalike/
//   everyone-quotes-command-line-arguments-the-wrong-way
void appendArgQuoted(std::wstring& cmd, std::wstring_view arg)
{
    if (!cmd.empty())
        cmd.push_back(L' ');

    auto needsQuoting = arg.empty()
                        || arg.find_first_of(L" \t\n\v\"")
                               != std::wstring_view::npos;

    if (!needsQuoting)
    {
        cmd.append(arg);
        return;
    }

    cmd.push_back(L'"');
    auto it = arg.begin();
    while (it != arg.end())
    {
        auto backslashes = 0;
        while (it != arg.end() && *it == L'\\')
        {
            ++backslashes;
            ++it;
        }

        if (it == arg.end())
        {
            // Trailing backslashes fall just before the closing
            // quote, so they must be doubled to survive parsing.
            cmd.append(static_cast<std::size_t>(backslashes * 2), L'\\');
            break;
        }
        if (*it == L'"')
        {
            cmd.append(static_cast<std::size_t>(backslashes * 2 + 1), L'\\');
            cmd.push_back(L'"');
        }
        else
        {
            cmd.append(static_cast<std::size_t>(backslashes), L'\\');
            cmd.push_back(*it);
        }
        ++it;
    }
    cmd.push_back(L'"');
}

std::wstring buildCommandLine(const std::string& executable,
                              const std::vector<std::string>& args)
{
    auto cmd = std::wstring {};
    appendArgQuoted(cmd, utf8ToWide(executable));
    for (auto& a: args)
        appendArgQuoted(cmd, utf8ToWide(a));
    return cmd;
}

struct WideCaseInsensitive
{
    bool operator()(const std::wstring& a, const std::wstring& b) const
    {
        return ::CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, a.c_str(),
                                static_cast<int>(a.size()), b.c_str(),
                                static_cast<int>(b.size()))
               == CSTR_LESS_THAN;
    }
};

using WideEnvMap = std::map<std::wstring, std::wstring, WideCaseInsensitive>;

WideEnvMap parentEnvW()
{
    auto result = WideEnvMap {};
    auto* env = ::GetEnvironmentStringsW();
    if (env == nullptr)
        return result;

    for (auto* p = env; *p != L'\0';)
    {
        auto entry = std::wstring_view {p};
        p += entry.size() + 1;

        // Drive-letter pseudo-vars start with '=' — skip them so the
        // sorted block CreateProcessW expects stays well-formed.
        if (entry[0] == L'=')
            continue;

        auto eq = entry.find(L'=');
        if (eq == std::wstring_view::npos)
            continue;

        result.emplace(std::wstring(entry.substr(0, eq)),
                       std::wstring(entry.substr(eq + 1)));
    }

    ::FreeEnvironmentStringsW(env);
    return result;
}

std::vector<wchar_t> buildEnvBlock(
    const std::map<std::string, std::string>& overrides)
{
    auto merged = parentEnvW();
    for (auto& [k, v]: overrides)
    {
        auto wk = utf8ToWide(k);
        if (v.empty())
            merged.erase(wk);
        else
            merged[wk] = utf8ToWide(v);
    }

    auto block = std::vector<wchar_t> {};
    for (auto& [k, v]: merged)
    {
        block.insert(block.end(), k.begin(), k.end());
        block.push_back(L'=');
        block.insert(block.end(), v.begin(), v.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

void readerLoop(HANDLE pipe, OutputState& state, bool isStdout)
{
    char chunk[4096];
    while (true)
    {
        DWORD bytesRead = 0;
        auto ok = ::ReadFile(pipe, chunk, sizeof chunk, &bytesRead, nullptr);
        if (!ok || bytesRead == 0)
            break;

        auto view =
            std::string_view {chunk, static_cast<std::size_t>(bytesRead)};
        if (isStdout)
            state.appendOut(view);
        else
            state.appendErr(view);
    }
    if (isStdout)
        state.markOutEof();
    else
        state.markErrEof();
}

} // namespace

struct Process::Impl
{
    HANDLE processHandle = nullptr;
    HANDLE stdoutRead = nullptr;
    HANDLE stderrRead = nullptr;
    std::thread stdoutReader;
    std::thread stderrReader;
    OutputState output;
    bool reaped = false;
    int exitCode = 0;

    void startReaders()
    {
        stdoutReader =
            std::thread {[this] { readerLoop(stdoutRead, output, true); }};
        stderrReader =
            std::thread {[this] { readerLoop(stderrRead, output, false); }};
    }

    void joinReaders()
    {
        if (stdoutReader.joinable())
            stdoutReader.join();
        if (stderrReader.joinable())
            stderrReader.join();
    }
};

Process::Process(const std::string& executable, const ProcessOptions& options)
    : impl(std::make_unique<Impl>())
{
    auto sa = SECURITY_ATTRIBUTES {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outRead = nullptr;
    HANDLE outWrite = nullptr;
    HANDLE errRead = nullptr;
    HANDLE errWrite = nullptr;

    if (!::CreatePipe(&outRead, &outWrite, &sa, 0))
        throw std::runtime_error("Process: CreatePipe (stdout) failed");
    if (!::CreatePipe(&errRead, &errWrite, &sa, 0))
    {
        ::CloseHandle(outRead);
        ::CloseHandle(outWrite);
        throw std::runtime_error("Process: CreatePipe (stderr) failed");
    }

    // Parent's read ends must NOT be inheritable, so the child
    // process doesn't accidentally keep a copy alive and block our
    // EOF detection when the real writer end closes.
    ::SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

    auto nul = ::CreateFileW(L"NUL", GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE)
    {
        ::CloseHandle(outRead);
        ::CloseHandle(outWrite);
        ::CloseHandle(errRead);
        ::CloseHandle(errWrite);
        throw std::runtime_error("Process: CreateFileW(NUL) failed");
    }

    auto si = STARTUPINFOW {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = outWrite;
    si.hStdError = errWrite;

    auto pi = PROCESS_INFORMATION {};

    auto cmdLine = buildCommandLine(executable, options.args);
    auto cwdW = utf8ToWide(options.workingDirectory);
    auto envBlock = buildEnvBlock(options.env);

    auto ok = ::CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE,
                               CREATE_UNICODE_ENVIRONMENT, envBlock.data(),
                               options.workingDirectory.empty() ? nullptr
                                                                : cwdW.c_str(),
                               &si, &pi);

    ::CloseHandle(outWrite);
    ::CloseHandle(errWrite);
    ::CloseHandle(nul);

    if (!ok)
    {
        auto err = ::GetLastError();
        ::CloseHandle(outRead);
        ::CloseHandle(errRead);
        throw std::runtime_error("Process: CreateProcessW '" + executable
                                 + "' failed (GetLastError="
                                 + std::to_string(err) + ")");
    }

    ::CloseHandle(pi.hThread);

    impl->processHandle = pi.hProcess;
    impl->stdoutRead = outRead;
    impl->stderrRead = errRead;
    impl->startReaders();
}

Process::~Process()
{
    if (impl == nullptr)
        return;

    if (impl->processHandle != nullptr && !impl->reaped)
        terminate();

    impl->joinReaders();

    if (impl->stdoutRead != nullptr)
        ::CloseHandle(impl->stdoutRead);
    if (impl->stderrRead != nullptr)
        ::CloseHandle(impl->stderrRead);
    if (impl->processHandle != nullptr)
        ::CloseHandle(impl->processHandle);
}

std::size_t Process::pollOutput(std::chrono::milliseconds timeout)
{
    auto& s = impl->output;
    auto lock = std::unique_lock {s.mutex};

    auto outBefore = s.outBuf.size();
    auto errBefore = s.errBuf.size();

    s.cv.wait_for(lock, timeout,
                  [&]
                  {
                      return s.outBuf.size() > outBefore
                          || s.errBuf.size() > errBefore
                          || (s.outEof && s.errEof);
                  });

    return (s.outBuf.size() - outBefore) + (s.errBuf.size() - errBefore);
}

std::string Process::stdoutBuffer() const
{
    auto lock = std::lock_guard {impl->output.mutex};
    return impl->output.outBuf;
}

std::string Process::stderrBuffer() const
{
    auto lock = std::lock_guard {impl->output.mutex};
    return impl->output.errBuf;
}

bool Process::hasExited()
{
    if (impl->reaped)
        return true;

    DWORD code = 0;
    if (!::GetExitCodeProcess(impl->processHandle, &code))
        return false;
    if (code == STILL_ACTIVE)
        return false;

    impl->exitCode = static_cast<int>(code);
    impl->reaped = true;
    return true;
}

void Process::terminate(std::chrono::milliseconds /*gracePeriod*/)
{
    if (impl->reaped)
        return;

    // No portable polite-stop primitive exists for a non-console GUI
    // child on Windows. The WebView test child has no persistent
    // state to flush, so TerminateProcess is safe and matches the
    // Apple SIGKILL fallback behaviour.
    ::TerminateProcess(impl->processHandle, 1);
    ::WaitForSingleObject(impl->processHandle, INFINITE);

    DWORD code = 0;
    if (::GetExitCodeProcess(impl->processHandle, &code))
        impl->exitCode = static_cast<int>(code);
    impl->reaped = true;
}

int Process::exitCode() const
{
    return impl->exitCode;
}

int Process::terminatingSignal() const
{
    return 0;
}

} // namespace eacp::WebView::Test
