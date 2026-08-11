#import <Carbon/Carbon.h>

#include "GlobalHotKey.h"

#include <eacp/Core/Utils/Singleton.h>

#include <map>

namespace eacp::Graphics
{
namespace
{
// Hotkey events reach every installed application handler, so each eacp copy
// needs its own signature to claim only its own registrations.
OSType hotKeySignature()
{
    static const auto signature = []
    {
        auto result = OSType {'eHKy'};

        for (auto c: Plugins::getModuleIdentitySuffix())
            result = result * 31 + (unsigned char) c;

        return result;
    }();

    return signature;
}

// Immortal because a GlobalHotKey at namespace scope can outlive it and
// ~Native erases itself here.
std::map<UInt32, Callback*>& registry()
{
    return Singleton::getImmortal<std::map<UInt32, Callback*>>();
}

UInt32 nextHotKeyID()
{
    static UInt32 nextID = 1;
    return nextID++;
}

UInt32 toCarbonModifiers(ModifierKeys modifiers)
{
    auto flags = UInt32 {0};

    if (modifiers.shift)
        flags |= shiftKey;
    if (modifiers.control)
        flags |= controlKey;
    if (modifiers.alt)
        flags |= optionKey;
    if (modifiers.command)
        flags |= cmdKey;

    return flags;
}

OSStatus hotKeyHandler(EventHandlerCallRef, EventRef event, void*)
{
    auto hotKeyID = EventHotKeyID {};

    auto status = GetEventParameter(event,
                                    kEventParamDirectObject,
                                    typeEventHotKeyID,
                                    nullptr,
                                    sizeof(hotKeyID),
                                    nullptr,
                                    &hotKeyID);

    if (status != noErr || hotKeyID.signature != hotKeySignature())
        return eventNotHandledErr;

    auto found = registry().find(hotKeyID.id);
    if (found == registry().end() || found->second == nullptr
        || !(*found->second))
        return eventNotHandledErr;

    auto callback = *found->second;
    callback();
    return noErr;
}

bool installHandlerOnce()
{
    static bool installed = false;
    if (installed)
        return true;

    auto eventType = EventTypeSpec {kEventClassKeyboard, kEventHotKeyPressed};
    auto status = InstallApplicationEventHandler(NewEventHandlerUPP(hotKeyHandler),
                                                 1,
                                                 &eventType,
                                                 nullptr,
                                                 nullptr);
    if (status != noErr)
    {
        LOG("GlobalHotKey handler installation failed: status ", status);
        return false;
    }

    installed = true;
    return true;
}
} // namespace

struct GlobalHotKey::Native
{
    Native(ModifierKeys modifiers, uint16_t keyCode, Callback callback)
        : onPressed(std::move(callback))
    {
        if (eacp::Apps::getAppEnvironment().headless)
            return;

        if (!installHandlerOnce())
            return;

        id = nextHotKeyID();
        auto eventID = EventHotKeyID {hotKeySignature(), id};

        auto status = RegisterEventHotKey(keyCode,
                                          toCarbonModifiers(modifiers),
                                          eventID,
                                          GetApplicationEventTarget(),
                                          0,
                                          &hotKeyRef);

        if (status == noErr)
        {
            registry()[id] = &onPressed;
        }
        else
        {
            LOG("GlobalHotKey registration failed: status ", status);
            hotKeyRef = nullptr;
            id = 0;
        }
    }

    ~Native()
    {
        if (id != 0)
            registry().erase(id);

        if (hotKeyRef != nullptr)
            UnregisterEventHotKey(hotKeyRef);
    }

    EventHotKeyRef hotKeyRef = nullptr;
    UInt32 id = 0;
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
