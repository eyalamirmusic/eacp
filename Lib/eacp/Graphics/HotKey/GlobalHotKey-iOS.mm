#include "GlobalHotKey.h"

#include <utility>

namespace eacp::Graphics
{

struct GlobalHotKey::Native
{
    Native(ModifierKeys, uint16_t, Callback callback)
        : onPressed(std::move(callback))
    {
    }

    Callback onPressed;
};

GlobalHotKey::GlobalHotKey(ModifierKeys modifiers,
                           uint16_t keyCode,
                           Callback onPressed)
    : impl(modifiers, keyCode, std::move(onPressed))
{
}

GlobalHotKey::~GlobalHotKey() = default;

} // namespace eacp::Graphics
