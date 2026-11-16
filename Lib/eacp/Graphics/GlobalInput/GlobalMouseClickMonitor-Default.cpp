#include "GlobalMouseClickMonitor.h"

namespace eacp::Graphics
{

struct GlobalMouseClickMonitor::Native
{
    explicit Native(Callback) {}
};

GlobalMouseClickMonitor::GlobalMouseClickMonitor(Callback onPressed)
    : impl(std::move(onPressed))
{
}

GlobalMouseClickMonitor::~GlobalMouseClickMonitor() = default;

} // namespace eacp::Graphics
