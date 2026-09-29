#include "KeyEchoes.h"

namespace eacp::Graphics
{

namespace
{
struct ForwardedKey
{
    double timestamp = 0.0;
    uint32_t nativeKey = 0;
    KeyEventType type = KeyEventType::Down;

    bool operator==(const ForwardedKey&) const = default;
};

constexpr auto rememberedForwardedKeys = 16;

Vector<ForwardedKey>& recentlyForwardedKeys()
{
    static auto keys = Vector<ForwardedKey> {};
    return keys;
}

ForwardedKey identityOf(const NativeKeyEvent& event)
{
    return {event.key.timestamp, event.nativeKey, event.key.type};
}

void rememberForwarded(const NativeKeyEvent& event)
{
    auto& keys = recentlyForwardedKeys();

    if (keys.size() == rememberedForwardedKeys)
        keys.erase(keys.begin());

    keys.add(identityOf(event));
}
} // namespace

bool isEchoOfForwardedKey(const NativeKeyEvent& event)
{
    return recentlyForwardedKeys().contains(identityOf(event));
}

EmbedderKeyForwarder::EmbedderKeyForwarder(View& fromToUse)
    : from(fromToUse)
{
}

void EmbedderKeyForwarder::forward(const NativeKeyEvent& event)
{
    if (isEchoOfForwardedKey(event) || !pairsWithForwardedDown(event))
        return;

    rememberForwarded(event);
    deliver(event);
}

void EmbedderKeyForwarder::forwardUnpaired(const NativeKeyEvent& event,
                                           const Claim& claim)
{
    if (isEchoOfForwardedKey(event))
        return;

    rememberForwarded(event);

    if (!claim(event.key))
        deliver(event);
}

bool EmbedderKeyForwarder::pairsWithForwardedDown(const NativeKeyEvent& event)
{
    if (event.key.type == KeyEventType::Down)
    {
        forwardedDowns.addIfNotThere(event.nativeKey);
        return true;
    }

    return forwardedDowns.removeAllMatches(event.nativeKey) > 0;
}

} // namespace eacp::Graphics
