#include "GameInputBackend.h"

namespace eacp::Graphics
{
std::unique_ptr<GameInputBackend> makeGameInputBackend(GameInputQueue&,
                                                       const std::atomic<bool>&)
{
    return nullptr;
}
} // namespace eacp::Graphics
