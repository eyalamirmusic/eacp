#include "PrefixSumKernels.h"

namespace eacp::GPUWidgets
{
ScanBlockKernel::ScanBlockKernel()
{
    compile();
}

void ScanBlockKernel::define()
{
    auto lane = localId();
    auto base = var(groupId() * (unsigned) perGroup + lane * (unsigned) perLane);

    // What this thread's own sixteen come to, which is the only thing the group
    // has to agree about before it can scan.
    auto mine = var(0u);

    for (auto i = 0; i < perLane; ++i)
    {
        auto at = base.get() + (unsigned) i;
        ifThen(at < elementCount, [&] { mine += counts.load(at); });
    }

    auto subtotals = shared<GPU::UInt>(lanes);
    write(subtotals, lane, mine.get());
    barrier();

    // Six doublings turn the subtotals into their own inclusive prefix. The C++
    // loop is unrolled as the graph is built, so the barriers below are
    // statements of the kernel rather than iterations of one - which they have
    // to be, a barrier inside a loop some threads leave early being undefined
    // on both backends.
    for (auto step = 1; step < lanes; step <<= 1)
    {
        auto carried = var(0u);

        ifThen(lane >= (unsigned) step,
               [&]
               {
                   // Clamped rather than trusted: the subscript is inside the
                   // guard, but an index that underflows when the guard is
                   // false is a shape worth not writing down at all.
                   carried = subtotals[max(lane, (unsigned) step) - (unsigned) step];
               });

        barrier();
        write(subtotals, lane, subtotals[lane] + carried.get());
        barrier();
    }

    // Inclusive minus its own is exclusive, which is where this thread's run of
    // sixteen starts.
    auto running = var(subtotals[lane] - mine.get());

    for (auto i = 0; i < perLane; ++i)
    {
        auto at = base.get() + (unsigned) i;

        ifThen(at < elementCount,
               [&]
               {
                   auto value = counts.load(at);
                   write(offsets, at, running.get());
                   running += value;
                   write(counts, at, 0u);
               });
    }

    // The group's total, for the level above to scan. Written by the lane
    // holding the inclusive sum of all of them, which is the last.
    ifThen(lane == (unsigned) (lanes - 1),
           [&]
           { write(groupTotals, groupId(), subtotals[(unsigned) (lanes - 1)]); });
}

ScanAddKernel::ScanAddKernel()
{
    compile();
}

void ScanAddKernel::define()
{
    auto at = threadId();
    auto group = at / (unsigned) ScanBlockKernel::perGroup;

    write(offsets, at, offsets.load(at) + groupOffsets.load(group));
}
} // namespace eacp::GPUWidgets
