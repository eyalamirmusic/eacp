#include "CrashHandler.h"

namespace eacp::CrashReporting
{
namespace
{
CrashHandlerFactory& getCrashHandlerFactory()
{
    static auto factory =
        CrashHandlerFactory {[] { return std::make_unique<CrashHandler>(); }};

    return factory;
}
} // namespace

std::unique_ptr<CrashHandler> createRegisteredCrashHandler()
{
    return getCrashHandlerFactory()();
}

Detail::CrashHandlerRegistration::CrashHandlerRegistration(
    CrashHandlerFactory factory)
{
    getCrashHandlerFactory() = std::move(factory);
}
} // namespace eacp::CrashReporting
