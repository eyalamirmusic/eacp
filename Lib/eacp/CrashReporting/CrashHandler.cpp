#include "CrashHandler.h"

namespace eacp::CrashReporting
{
namespace
{
CrashHandlerFactory& getCrashHandlerFactory()
{
    static auto factory =
        CrashHandlerFactory {[] { return makeOwned<CrashHandler>(); }};

    return factory;
}
} // namespace

OwningPointer<CrashHandler> createRegisteredCrashHandler()
{
    return getCrashHandlerFactory()();
}

Detail::CrashHandlerRegistration::CrashHandlerRegistration(
    CrashHandlerFactory factory)
{
    getCrashHandlerFactory() = std::move(factory);
}
} // namespace eacp::CrashReporting
