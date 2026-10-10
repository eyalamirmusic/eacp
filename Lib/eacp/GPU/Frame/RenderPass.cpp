#include "RenderPass.h"

#include <eacp/Core/Utils/Logging.h>

// The participant list, shared by both backends. Only the encoder itself is
// platform-specific; when a queued draw gets its last chance to reach one is a
// rule of the API, and a rule the two backends must not be able to disagree
// about — so it lives here rather than twice in RenderPass-Apple.mm and
// RenderPass-Windows.cpp, which call drainParticipants() from end().

namespace eacp::GPU
{
void RenderPass::addParticipant(Participant& participant)
{
    // Joining twice would flush the same queue twice, and the second flush is a
    // draw of nothing - harmless, but it would also mean a participant could
    // not tell how many times it is going to be called.
    participants.addIfNotThere(&participant);
}

void RenderPass::removeParticipant(Participant& participant)
{
    participants.removeAllMatches(&participant);
}

void RenderPass::drainParticipants()
{
    if (drained)
        return;

    // Set before the walk, not after: a participant flushing issues draws on
    // this pass, and a draw must not be able to start the drain over again.
    drained = true;

    for (auto* participant: participants)
        participant->flushInto(*this);

    // Each has had its one chance; nothing may hold a pointer to a pass that is
    // closing.
    participants.clear();
}

void RenderPass::drawIndirect(const Buffer& arguments, std::int64_t offset)
{
    drawIndirect(arguments, offset, 1);
}

void RenderPass::drawIndexedIndirect(const Buffer& indices,
                                     IndexFormat format,
                                     const Buffer& arguments,
                                     std::int64_t offset)
{
    drawIndexedIndirect(BufferRange::of(indices), format, arguments, offset, 1);
}

void RenderPass::drawIndexedIndirect(const BufferRange& indices,
                                     IndexFormat format,
                                     const Buffer& arguments,
                                     std::int64_t offset)
{
    drawIndexedIndirect(indices, format, arguments, offset, 1);
}

// The rules of beginOcclusionQuery, checked once here so that no backend can
// open a second query (Metal would move the first one's offset, Vulkan and
// D3D12 reject it) or count one index twice in a pass (a Vulkan query needs a
// reset between, which only happens outside a pass).
void RenderPass::beginOcclusionQuery(int index, OcclusionMode mode)
{
    if (occlusionQueries == nullptr)
    {
        LOG("eacp: beginOcclusionQuery on a pass whose descriptor named no "
            "valid OcclusionQueries");
        return;
    }

    if (index < 0 || index >= occlusionQueries->size())
    {
        LOG("eacp: occlusion query ",
            index,
            " is outside a set of ",
            occlusionQueries->size());
        return;
    }

    if (openQuery >= 0)
    {
        LOG("eacp: occlusion query ",
            index,
            " begun while query ",
            openQuery,
            " is still open; one is open at a time");
        return;
    }

    for (const auto& used: usedQueries)
    {
        if (used.index == index)
        {
            LOG("eacp: occlusion query ",
                index,
                " was already counted in this pass; an index is begun once a "
                "pass");
            return;
        }
    }

    nativeBeginOcclusionQuery(index, mode);
    usedQueries.add(UsedQuery {index, mode});
    openQuery = index;
}

void RenderPass::endOcclusionQuery()
{
    if (openQuery < 0)
        return;

    nativeEndOcclusionQuery(openQuery, usedQueries.back().mode);
    openQuery = -1;
}
} // namespace eacp::GPU
