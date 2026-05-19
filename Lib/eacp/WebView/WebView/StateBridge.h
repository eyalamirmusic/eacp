#pragma once

#include "../../Core/Utils/StateValue.h"

#include <Miro/Miro.h>

#include <string>
#include <utility>

namespace eacp::Graphics
{

template <typename T>
eacp::Listener bindToBridge(eacp::StateValue<T>& state,
                            Miro::Bridge& bridge,
                            std::string eventName)
{
    return eacp::Listener {
        state.onChange(),
        [&state, &bridge, name = std::move(eventName)]
        { bridge.emit(name, state.get()); }};
}

} // namespace eacp::Graphics
