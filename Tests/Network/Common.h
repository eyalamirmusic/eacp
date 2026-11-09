#pragma once

#include <eacp/Core/Utils/StdPath.h>
#include <eacp/Network/Network.h>
#include <Miro/Reflect.h>
#include <NanoTest/NanoTest.h>

#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

// Only a guard against a wedged fetch, never a speed bound: a stalled CI
// runner has stretched a loopback fetch that takes milliseconds past 5 s.
constexpr auto fetchTimeout = eacp::Time::MS {30000};

// Every server here binds port 0 and reports back what it got. Fixed ports do
// not survive this suite: NanoTest's CTest discovery runs each case in its own
// process, so a per-process counter hands every concurrent case the same number,
// and SO_REUSEADDR lets them all bind it and steal each other's connections.
inline std::string baseUrl(int port)
{
    return "http://127.0.0.1:" + std::to_string(port);
}

inline std::string tempPath(const std::string& name)
{
    auto path = std::filesystem::temp_directory_path() / ("eacp-" + name);
    auto ec = std::error_code();
    std::filesystem::remove(path, ec);
    return path.string();
}

inline eacp::FilePath scratchDirectory(const std::string& name)
{
    auto path = std::filesystem::temp_directory_path() / ("eacp-" + name);
    auto ignored = std::error_code {};
    std::filesystem::remove_all(path, ignored);
    return eacp::FilePath {path};
}

inline std::string readFile(const std::string& path)
{
    auto file = std::ifstream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

inline bool exists(const eacp::FilePath& path)
{
    return std::filesystem::exists(eacp::toStdPath(path));
}

// A handler that blocks has to run off the event loop, which is the same
// loop the reply has to travel back through.
inline eacp::HTTP::ServerOptions threadPoolOptions()
{
    auto options = eacp::HTTP::ServerOptions();
    options.threading = eacp::HTTP::ServerThreadingMode::ThreadPool;
    options.threadPoolSize = 2;
    return options;
}

inline bool runOffTheLoop(const std::function<void()>& work,
                          eacp::Time::MS budget = fetchTimeout)
{
    auto worker = std::thread();

    auto startWorker = [&]
    {
        auto runAndStop = [&]
        {
            work();
            eacp::Threads::callAsync([] { eacp::Threads::stopEventLoop(); });
        };

        worker = std::thread(runAndStop);
    };

    auto stopped = eacp::Threads::runEventLoopFor(budget, startWorker);

    if (worker.joinable())
        worker.join();

    return stopped;
}

inline eacp::HTTP::Response performOffTheLoop(eacp::HTTP::Server& server,
                                              const eacp::HTTP::Request& req)
{
    auto response = eacp::HTTP::Response();
    auto perform = [&] { response = req.perform(); };
    nano::check(runOffTheLoop(perform));
    server.stop();
    return response;
}

struct Exchange
{
    eacp::HTTP::Response clientResponse;
    bool completed = false;
};

inline void performExchange(eacp::HTTP::Server& server,
                            const eacp::HTTP::Request& clientRequest,
                            Exchange& out)
{
    auto worker = std::thread();

    auto stopped = eacp::Threads::runEventLoopFor(
        eacp::Time::MS {5000},
        [&]
        {
            worker = std::thread(
                [&]
                {
                    out.clientResponse = eacp::HTTP::httpRequest(clientRequest);
                    eacp::Threads::callAsync([] { eacp::Threads::stopEventLoop(); });
                });
        });

    worker.join();
    out.completed = stopped;
    server.stop();
}

struct GreetRequest
{
    std::string name;
    MIRO_REFLECT(name)
};

struct GreetResponse
{
    std::string greeting;
    MIRO_REFLECT(greeting)
};

// A bound for calls that must not wait for a server that is deliberately
// stalled - the client's own timeout, or the fact that a request is
// asynchronous, is what has to end them. The stalled handlers never answer
// while the assertions run, so any completion at all already proves that; the
// bound only has to sit clear of the stalls a shared runner inflicts on every
// process at once. Windows CI showed how far that reaches: in one burst a
// pure-logic test went from 0.01s to 0.36s and a 250ms-limited request from
// 0.30s to just over a second, which is where the previous 1000ms bound
// failed.
constexpr auto withoutWaitingForTheServer = eacp::Time::MS {3000};

// A server handler that has to still be in flight when the client gives up
// blocks here instead of sleeping for a fixed span. The test releases it once
// it has asserted, so teardown never waits out the tail of a sleep that has
// already served its purpose - which is where these suites used to spend most
// of their time. The bound on wait() is a backstop against a test that fails
// before releasing, not a timing the tests rely on.
struct StallGate
{
    ~StallGate() { release(); }

    void wait()
    {
        auto lock = std::unique_lock(mutex);
        opened.wait_for(lock, std::chrono::seconds(10), [this] { return isOpen; });
    }

    void release()
    {
        {
            auto lock = std::scoped_lock(mutex);
            isOpen = true;
        }

        opened.notify_all();
    }

    std::mutex mutex;
    std::condition_variable opened;
    bool isOpen = false;
};
