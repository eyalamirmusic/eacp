#pragma once

#include <Miro/Miro.h>
#include <eacp/Core/Threads/Async.h>

#include <chrono>
#include <cmath>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace eacp;

struct SearchRequest
{
    std::string query;

    MIRO_REFLECT(query)
};

struct SearchHit
{
    std::string title;
    double score = 0.0;

    MIRO_REFLECT(title, score)
};

struct SearchResults
{
    std::string query;
    std::vector<SearchHit> hits;

    MIRO_REFLECT(query, hits)
};

namespace Api
{

// Realtime search: the kind of feature that's miserable when the command
// blocks the UI thread. Two commands run the SAME 200ms "backend" query
// and generate the same shape of results — the only difference is how
// they wait:
//
//   search          → async. A coroutine that co_awaits Threads::delay,
//                     so the 200ms passes off the main thread and the
//                     bridge replies only when it settles. The event loop
//                     stays free: many searches overlap and resolve at
//                     ~200ms regardless of how many are in flight.
//
//   searchBlocking  → synchronous. The same 200ms, but spent sleeping on
//                     the calling (main) thread. Each call hogs the event
//                     loop for its whole duration, so concurrent calls
//                     serialise — N of them take N×200ms, and nothing else
//                     on the main thread runs in the meantime.
//
// Both register identically via r.commands<...>(); the bridge awaits the
// async one and replies inline for the blocking one. The demo benchmarks
// them head to head so the difference is measurable, not hand-waved.
class SearchApi
{
public:
    void reflect(Miro::ApiReflector& r)
    {
        r.commands<&SearchApi::search, &SearchApi::searchBlocking>();
    }

    Threads::Async<SearchResults> search(const SearchRequest& req)
    {
        co_await Threads::delay(std::chrono::milliseconds {200});
        co_return generate(req);
    }

    SearchResults searchBlocking(const SearchRequest& req)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds {200});
        return generate(req);
    }

private:
    SearchResults generate(const SearchRequest& req)
    {
        auto results = SearchResults {};
        results.query = req.query;

        if (!req.query.empty())
        {
            auto count = countDist(rng);
            for (auto i = 0; i < count; ++i)
            {
                auto hit = SearchHit {};
                hit.title = req.query + " — result " + std::to_string(i + 1);
                hit.score = std::round(scoreDist(rng) * 100.0) / 100.0;
                results.hits.push_back(std::move(hit));
            }
        }

        return results;
    }

    std::mt19937 rng {std::random_device {}()};
    std::uniform_int_distribution<int> countDist {3, 8};
    std::uniform_real_distribution<double> scoreDist {0.4, 1.0};
};

} // namespace Api
