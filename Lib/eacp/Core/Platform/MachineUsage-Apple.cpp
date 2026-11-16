#include "MachineUsagePlatform.h"

#include <mach/mach.h>
#include <sys/sysctl.h>

namespace eacp::Platform
{
namespace
{
class MachHostPort
{
public:
    MachHostPort() = default;
    MachHostPort(const MachHostPort&) = delete;
    MachHostPort& operator=(const MachHostPort&) = delete;

    ~MachHostPort() { mach_port_deallocate(mach_task_self(), port); }

    host_t get() const { return port; }

private:
    host_t port = mach_host_self();
};

std::uint64_t readPhysicalMemoryBytes()
{
    auto bytes = std::uint64_t {0};
    auto size = sizeof(bytes);

    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) != 0)
        return 0;

    return bytes;
}

std::uint64_t readProcessFootprintBytes()
{
    auto info = task_vm_info_data_t {};
    auto count = mach_msg_type_number_t {TASK_VM_INFO_COUNT};
    auto* infoOut = reinterpret_cast<task_info_t>(&info);

    if (task_info(mach_task_self(), TASK_VM_INFO, infoOut, &count) != KERN_SUCCESS)
        return 0;

    return info.phys_footprint;
}

// Activity Monitor's "Memory Used": app memory (anonymous pages less the
// purgeable ones), wired and compressed.
std::uint64_t readUsedMemoryBytes()
{
    const auto host = MachHostPort {};
    auto statistics = vm_statistics64_data_t {};
    auto count = mach_msg_type_number_t {HOST_VM_INFO64_COUNT};
    auto* statisticsOut = reinterpret_cast<host_info64_t>(&statistics);

    if (host_statistics64(host.get(), HOST_VM_INFO64, statisticsOut, &count)
        != KERN_SUCCESS)
        return 0;

    const auto appPages = static_cast<std::uint64_t>(statistics.internal_page_count)
                          - statistics.purgeable_count;
    const auto usedPages =
        appPages + statistics.wire_count + statistics.compressor_page_count;

    return usedPages * vm_page_size;
}
} // namespace

std::optional<Detail::SystemCpuTicks> Detail::readSystemCpuTicks()
{
    const auto host = MachHostPort {};
    auto load = host_cpu_load_info_data_t {};
    auto count = mach_msg_type_number_t {HOST_CPU_LOAD_INFO_COUNT};
    auto* loadOut = reinterpret_cast<host_info_t>(&load);

    if (host_statistics(host.get(), HOST_CPU_LOAD_INFO, loadOut, &count)
        != KERN_SUCCESS)
        return {};

    const auto busy = static_cast<std::uint64_t>(load.cpu_ticks[CPU_STATE_USER])
                      + load.cpu_ticks[CPU_STATE_SYSTEM]
                      + load.cpu_ticks[CPU_STATE_NICE];

    return SystemCpuTicks {.busy = busy,
                           .total = busy + load.cpu_ticks[CPU_STATE_IDLE]};
}

MemoryUsage getMemoryUsage()
{
    return {.processBytes = readProcessFootprintBytes(),
            .systemUsedBytes = readUsedMemoryBytes(),
            .systemTotalBytes = readPhysicalMemoryBytes()};
}
} // namespace eacp::Platform
