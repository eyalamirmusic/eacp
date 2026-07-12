#include "HttpServerDispatcher.h"

#include <condition_variable>
#include <deque>
#include <thread>

namespace eacp::HTTP
{

namespace
{

void runTask(DispatchTask& task)
{
    auto response = task.handler ? task.handler(task.request) : Response();
    task.sendResponse(response);
}

struct EventLoopDispatcher : Dispatcher
{
    void dispatch(DispatchTask task) override
    {
        auto shared = std::make_shared<DispatchTask>(std::move(task));
        Threads::callAsync([shared] { runTask(*shared); });
    }

    void shutdown() override {}
};

struct ThreadPoolDispatcher : Dispatcher
{
    explicit ThreadPoolDispatcher(int poolSize)
    {
        auto count = std::max(1, poolSize);
        workers.reserve(count);
        for (auto i = 0; i < count; ++i)
            workers.emplace_back([this] { workerLoop(); });
    }

    ~ThreadPoolDispatcher() override { shutdown(); }

    void dispatch(DispatchTask task) override
    {
        {
            auto lock = std::lock_guard(mutex);
            if (stopping)
                return;
            queue.push_back(std::move(task));
        }
        cv.notify_one();
    }

    void shutdown() override
    {
        {
            auto lock = std::lock_guard(mutex);
            if (stopping)
                return;
            stopping = true;
        }
        cv.notify_all();

        for (auto& t: workers)
            if (t.joinable())
                t.join();
        workers.clear();
    }

private:
    void workerLoop()
    {
        while (true)
        {
            auto task = DispatchTask();
            {
                auto lock = std::unique_lock(mutex);
                cv.wait(lock, [this] { return stopping || !queue.empty(); });

                if (queue.empty())
                    return;

                task = std::move(queue.front());
                queue.pop_front();
            }
            runTask(task);
        }
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<DispatchTask> queue;
    Vector<std::thread> workers;
    bool stopping = false;
};

} // namespace

OwningPointer<Dispatcher> makeDispatcher(const ServerOptions& options)
{
    auto result = OwningPointer<Dispatcher>();

    if (options.threading == ServerThreadingMode::ThreadPool)
        result.create<ThreadPoolDispatcher>(options.threadPoolSize);
    else
        result.create<EventLoopDispatcher>();

    return result;
}

} // namespace eacp::HTTP
