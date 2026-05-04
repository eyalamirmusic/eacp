#pragma once

#include <Miro/Miro.h>

struct PingResponse
{
    bool pong = true;
    long long serverTimeMs = 0;

    MIRO_REFLECT(pong, serverTimeMs)
};
