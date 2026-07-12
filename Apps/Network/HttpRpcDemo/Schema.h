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

// Tiny RPC API. The static-init flow used a free `ping()` function +
// MIRO_EXPORT_COMMAND(ping); the inversion path uses a class whose
// reflect() body lists the same command.
//
// ping() is defined inline because the codegen executable instantiates
// the makePmfHandler lambda chain — that ODR-uses the function, and
// the codegen exe doesn't compile Schema.cpp.
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
