#include "GlobalMouseMonitor.h"

namespace eacp::Graphics
{

struct GlobalMouseMonitor::Native
{
    Native(EA::Vector<MouseEventType>, Handler) {}
};

GlobalMouseMonitor::GlobalMouseMonitor(EA::Vector<MouseEventType> typesToReport,
                                       Handler onEvent)
    : impl(std::move(typesToReport), std::move(onEvent))
{
}

GlobalMouseMonitor::~GlobalMouseMonitor() = default;

} // namespace eacp::Graphics
