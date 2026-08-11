#include "RenderPass.h"

namespace eacp::GPU
{
void RenderPass::addParticipant(Participant& participant)
{
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

    // Set before the walk: a participant's draws must not restart the drain.
    drained = true;

    for (auto* participant: participants)
        participant->flushInto(*this);

    participants.clear();
}
} // namespace eacp::GPU
