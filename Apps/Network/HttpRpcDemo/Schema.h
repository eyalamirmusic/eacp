#pragma once

#include <Miro/Bridge.h>
#include <Miro/Reflect.h>

#include <chrono>

struct PingResponse
{
    bool pong = false;
    long long serverTimeMs = 0;

    MIRO_REFLECT(pong, serverTimeMs)
};

namespace Api
{

// ping() must be inline: the codegen executable ODR-uses it through the
// makePmfHandler lambda chain but does not compile Schema.cpp.
class PingApi
{
public:
    void reflect(Miro::ApiReflector& r) { r.command(&PingApi::ping, "ping"); }

    PingResponse ping() const
    {
        using namespace std::chrono;
        auto now = system_clock::now().time_since_epoch();
        return {.pong = true,
                .serverTimeMs = duration_cast<milliseconds>(now).count()};
    }
};

} // namespace Api
