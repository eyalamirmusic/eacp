#import <Carbon/Carbon.h>
#import <Foundation/Foundation.h>

#include "GlobalHotKey.h"

#include <eacp/Core/ObjC/CFRef.h>
#include <eacp/Core/Utils/Singleton.h>

#include <map>

namespace eacp::Graphics
{
namespace
{
// Derived from the module identity: hotkey events are delivered to every
// installed application handler, so each eacp copy needs its own signature
// to claim only its own registrations.
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

// Immortal because ~Native erases itself here, and a GlobalHotKey at namespace
// scope outlives a registry that first use constructed after it.
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

// The system's own shortcuts win over any app's hotkey, so an enabled one on
// the same chord is a GlobalHotKey that would never fire. Only the four
// modifiers a chord can name are compared: the entries carry other bits too.
bool GlobalHotKey::isAvailable(ModifierKeys modifiers, uint16_t keyCode)
{
    auto raw = CFArrayRef {nullptr};

    if (CopySymbolicHotKeys(&raw) != noErr || raw == nullptr)
        return true;

    auto shortcuts = CFRef<CFArrayRef> {raw};
    auto wanted = toCarbonModifiers(modifiers);
    auto relevant = UInt32 {shiftKey | controlKey | optionKey | cmdKey};

    for (NSDictionary* shortcut in (__bridge NSArray*) shortcuts.get())
    {
        auto enabled =
            [shortcut[(__bridge NSString*) kHISymbolicHotKeyEnabled] boolValue];
        auto code =
            [shortcut[(__bridge NSString*) kHISymbolicHotKeyCode] unsignedIntValue];
        auto mods = [shortcut[(__bridge NSString*) kHISymbolicHotKeyModifiers]
            unsignedIntValue];

        if (enabled && code == keyCode && (mods & relevant) == wanted)
            return false;
    }

    return true;
}

} // namespace eacp::Graphics
