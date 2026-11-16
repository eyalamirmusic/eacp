#pragma once

#include <cstdint>
#include <string>

namespace eacp::GPU
{
// The GPU a Device runs on, as a crash report needs it named. A field the
// backend cannot answer keeps its default: Metal has no PCI vendor or device id,
// and its driver is the operating system's version.
struct AdapterDescription
{
    std::string name;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint64_t dedicatedMemoryBytes = 0;
    std::string driverVersion;
    std::string apiLevel;
    bool isSoftware = false;
};

// "NVIDIA RTX 6000 Ada Generation, vendor 0x10de, device 0x26b1, 49140 MB
// dedicated, driver 32.0.15.6094, D3D12 feature level 12_1, hardware"
std::string describeAdapterForLog(const AdapterDescription& adapter);
} // namespace eacp::GPU
