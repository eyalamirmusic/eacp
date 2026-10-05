#include "Common.h"

#include <eacp/GPU/Vulkan/VulkanLegacySync.h>

using namespace nano;
using namespace eacp::GPU;

// The bits synchronization2 added above bit 32 have no legacy counterpart at
// the same position, so a cast would drop them: a copy's barrier would then
// order nothing.
auto tTheSynchronization2OnlyStagesMapToTheirLegacyStage =
    test("LegacySync/theSynchronization2OnlyStagesMapToTheirLegacyStage") = []
{
    check(legacyStages(VK_PIPELINE_STAGE_2_COPY_BIT)
          == VK_PIPELINE_STAGE_TRANSFER_BIT);
    check(legacyStages(VK_PIPELINE_STAGE_2_RESOLVE_BIT)
          == VK_PIPELINE_STAGE_TRANSFER_BIT);
    check(legacyStages(VK_PIPELINE_STAGE_2_BLIT_BIT)
          == VK_PIPELINE_STAGE_TRANSFER_BIT);
    check(legacyStages(VK_PIPELINE_STAGE_2_CLEAR_BIT)
          == VK_PIPELINE_STAGE_TRANSFER_BIT);
    check(legacyStages(VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT)
          == VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
    check(legacyStages(VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT)
          == VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
    check(legacyStages(VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT)
          == VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
};

auto tTheSharedStagesMapBitForBit =
    test("LegacySync/theSharedStagesMapBitForBit") = []
{
    const std::pair<VkPipelineStageFlags2, VkPipelineStageFlags> shared[] = {
        {VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT},
        {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT},
        {VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT},
        {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT},
        {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT},
        {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT},
        {VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT},
        {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT},
        {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT},
        {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT},
        {VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT},
        {VK_PIPELINE_STAGE_2_HOST_BIT, VK_PIPELINE_STAGE_HOST_BIT},
        {VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT},
        {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT}};

    for (const auto& [modern, legacy]: shared)
        check(legacyStages(modern) == legacy);
};

auto tAMixedMaskTranslatesEveryBit =
    test("LegacySync/aMixedMaskTranslatesEveryBit") = []
{
    // barrierBeforeRendering's source scope.
    const auto mixed = VK_PIPELINE_STAGE_2_COPY_BIT
                       | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                       | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    check(legacyStages(mixed)
          == (VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
              | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT));
};

// NONE is legal on either side of a synchronization2 barrier and on neither of
// a legacy one.
auto tNoStageIsTopAsASourceAndBottomAsADestination =
    test("LegacySync/noStageIsTopAsASourceAndBottomAsADestination") = []
{
    check(legacySourceStages(VK_PIPELINE_STAGE_2_NONE)
          == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    check(legacyDestinationStages(VK_PIPELINE_STAGE_2_NONE)
          == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    check(legacySourceStages(VK_PIPELINE_STAGE_2_COPY_BIT)
          == VK_PIPELINE_STAGE_TRANSFER_BIT);
    check(legacyDestinationStages(VK_PIPELINE_STAGE_2_COPY_BIT)
          == VK_PIPELINE_STAGE_TRANSFER_BIT);
};

auto tTheSplitShaderReadsAndWritesMapToTheirLegacyAccess =
    test("LegacySync/theSplitShaderReadsAndWritesMapToTheirLegacyAccess") = []
{
    check(legacyAccess(VK_ACCESS_2_SHADER_SAMPLED_READ_BIT)
          == VK_ACCESS_SHADER_READ_BIT);
    check(legacyAccess(VK_ACCESS_2_SHADER_STORAGE_READ_BIT)
          == VK_ACCESS_SHADER_READ_BIT);
    check(legacyAccess(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)
          == VK_ACCESS_SHADER_WRITE_BIT);
    check(legacyAccess(VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                       | VK_ACCESS_2_SHADER_STORAGE_READ_BIT)
          == VK_ACCESS_SHADER_READ_BIT);
};

auto tTheSharedAccessMapsBitForBit =
    test("LegacySync/theSharedAccessMapsBitForBit") = []
{
    const std::pair<VkAccessFlags2, VkAccessFlags> shared[] = {
        {VK_ACCESS_2_NONE, VK_ACCESS_NONE},
        {VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT},
        {VK_ACCESS_2_INDEX_READ_BIT, VK_ACCESS_INDEX_READ_BIT},
        {VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT},
        {VK_ACCESS_2_UNIFORM_READ_BIT, VK_ACCESS_UNIFORM_READ_BIT},
        {VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT},
        {VK_ACCESS_2_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT},
        {VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT},
        {VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
        {VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT},
        {VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
        {VK_ACCESS_2_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT},
        {VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT},
        {VK_ACCESS_2_HOST_READ_BIT, VK_ACCESS_HOST_READ_BIT},
        {VK_ACCESS_2_HOST_WRITE_BIT, VK_ACCESS_HOST_WRITE_BIT},
        {VK_ACCESS_2_MEMORY_READ_BIT, VK_ACCESS_MEMORY_READ_BIT},
        {VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_WRITE_BIT}};

    for (const auto& [modern, legacy]: shared)
        check(legacyAccess(modern) == legacy);
};
