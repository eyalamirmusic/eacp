#include "StateBridge.h"

namespace eacp::Graphics
{

Vector<OwningPointer<EA::Listener>> attachStaticStateBinders(Miro::Bridge& bridge)
{
    auto listeners = Vector<OwningPointer<EA::Listener>> {};

    for (auto& binder: Detail::stateBinderRegistry())
        listeners.add(binder(bridge));

    return listeners;
}

} // namespace eacp::Graphics
