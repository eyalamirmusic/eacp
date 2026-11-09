#pragma once

#include <eacp/Core/Utils/Containers.h>

#include <algorithm>

namespace eacp::GPU
{
// When one timed pass had work in flight on the GPU, as up to two intervals in
// milliseconds from an origin every pass of the frame shares.
//
// Two because of Metal: on a tile-based GPU a render pass's vertex stage runs
// well ahead of its fragment stage - often while the passes before it are still
// shading - and the gap between the two is the pass waiting its turn, not
// working. A render pass there is its vertex and its fragment stage; everything
// else, on every backend, is one interval. A pass whose samples never arrived
// has none.
struct PassActivity
{
    struct Interval
    {
        double begin = 0.0;
        double end = 0.0;
    };

    void add(double begin, double end)
    {
        if (end >= begin && count < intervals.size())
            intervals[count++] = {begin, end};
    }

    Array<Interval, 2> intervals;
    int count = 0;
};

// Each pass's share of the frame, and its span.
//
// The share divides the GPU's busy time between the passes that were busy: the
// timeline is cut at every interval boundary, and each piece goes in equal parts
// to the passes with work in flight across it. A pass that ran alone is charged
// exactly its own time, two that overlapped split the overlap, and the shares
// add up to the time at least one timed pass was running - so, unlike the
// spans, they can be summed into a frame.
//
// The span is the first interval's start to the last one's end, waiting
// included. Where passes never overlap - D3D12 and Vulkan - it equals the share.
template <int MaxPasses>
void sharePassTimes(const PassActivity* activity,
                    int passCount,
                    double* shares,
                    double* spans)
{
    struct Edge
    {
        double time = 0.0;
        int pass = 0;
        int step = 0;
    };

    Array<Edge, MaxPasses * 4> edges;
    Array<int, MaxPasses> inFlight;

    auto edgeCount = 0;

    for (auto pass = 0; pass < passCount; ++pass)
    {
        const auto& own = activity[pass];

        shares[pass] = 0.0;
        spans[pass] = 0.0;
        inFlight[pass] = 0;

        if (own.count == 0)
            continue;

        auto first = own.intervals[0].begin;
        auto last = own.intervals[0].end;

        for (auto index = 0; index < own.count; ++index)
        {
            const auto& interval = own.intervals[index];

            first = std::min(first, interval.begin);
            last = std::max(last, interval.end);

            edges[edgeCount++] = {interval.begin, pass, 1};
            edges[edgeCount++] = {interval.end, pass, -1};
        }

        spans[pass] = last - first;
    }

    std::sort(edges.begin(),
              edges.begin() + edgeCount,
              [](const Edge& a, const Edge& b)
              { return a.time < b.time || (a.time == b.time && a.step > b.step); });

    // Counted per pass rather than per interval, so a pass whose two stages
    // overlap is still one pass sharing the piece and not two.
    auto running = 0;

    for (auto index = 0; index < edgeCount; ++index)
    {
        if (index > 0 && running > 0)
        {
            const auto piece = (edges[index].time - edges[index - 1].time) / running;

            for (auto pass = 0; pass < passCount; ++pass)
                if (inFlight[pass] > 0)
                    shares[pass] += piece;
        }

        const auto& edge = edges[index];
        const auto before = inFlight[edge.pass];

        inFlight[edge.pass] += edge.step;

        if (before == 0 && inFlight[edge.pass] > 0)
            ++running;
        else if (before > 0 && inFlight[edge.pass] == 0)
            --running;
    }
}
} // namespace eacp::GPU
