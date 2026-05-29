#include "Logging.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
    #include "WinInclude.h"
#endif

namespace eacp
{

namespace
{
struct LogState
{
    std::mutex mutex;
    std::string filePath;
    std::ofstream file;
};

LogState& state()
{
    static auto instance = LogState {};
    return instance;
}

std::string timestamp()
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch())
                      .count()
                  % 1000;

    auto tm = std::tm {};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    auto stream = std::ostringstream {};
    stream << std::put_time(&tm, "%H:%M:%S") << '.' << std::setfill('0')
           << std::setw(3) << millis;
    return stream.str();
}

#if defined(_WIN32)
void emitWindowsDebug(std::string_view line)
{
    auto withNewline = std::string {line};
    withNewline += '\n';

    auto required = MultiByteToWideChar(CP_UTF8,
                                        0,
                                        withNewline.data(),
                                        static_cast<int>(withNewline.size()),
                                        nullptr,
                                        0);
    if (required <= 0)
        return;

    auto wide = std::wstring(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        0,
                        withNewline.data(),
                        static_cast<int>(withNewline.size()),
                        wide.data(),
                        required);
    wide.push_back(L'\0');
    OutputDebugStringW(wide.c_str());
}
#endif

} // namespace

void LOG(std::string_view text)
{
    auto line = timestamp() + " | " + std::string {text};

    {
        auto guard = std::lock_guard {state().mutex};
        std::cout << line << std::endl;

        if (state().file.is_open())
        {
            state().file << line << '\n';
            state().file.flush();
        }
    }

#if defined(_WIN32)
    emitWindowsDebug(line);
#endif
}

void setLogFile(std::string_view path)
{
    auto guard = std::lock_guard {state().mutex};

    if (state().file.is_open())
        state().file.close();

    state().filePath.assign(path);
    if (state().filePath.empty())
        return;

    auto fsPath = std::filesystem::path {state().filePath};
    if (fsPath.has_parent_path())
    {
        auto ec = std::error_code {};
        std::filesystem::create_directories(fsPath.parent_path(), ec);
    }

    state().file.open(state().filePath, std::ios::app);
}

} // namespace eacp
