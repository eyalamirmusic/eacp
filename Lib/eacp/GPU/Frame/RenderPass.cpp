#include "RenderPass.h"

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
} // namespace eacp::GPU
