#pragma once

#include <volk.h>

// Synchronization2 on a device without it. The barrier, timestamp and submit
// entry points every call site uses are pointed at functions that record and
// submit through their Vulkan 1.0 counterparts, so no call site knows.

namespace eacp::GPU
{
// Bit for bit below bit 32, where the two flag types agree; the bits only
// synchronization2 has go to the legacy stage that contains them.
VkPipelineStageFlags legacyStages(VkPipelineStageFlags2 stages);

// NONE is legal in a synchronization2 scope and not in a legacy one: as the
// first scope it is TOP_OF_PIPE, which waits for nothing, and as the second
// BOTTOM_OF_PIPE, which blocks nothing.
VkPipelineStageFlags legacySourceStages(VkPipelineStageFlags2 stages);
VkPipelineStageFlags legacyDestinationStages(VkPipelineStageFlags2 stages);

VkAccessFlags legacyAccess(VkAccessFlags2 access);

// Replaces volk's vkCmdPipelineBarrier2, vkCmdWriteTimestamp2 and
// vkQueueSubmit2 once the device is loaded.
void installLegacySynchronization();
} // namespace eacp::GPU
