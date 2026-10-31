#pragma once

namespace SelfDestruct
{
// Stops AppKit's reopen-windows alert after repeated launch crashes from
// swallowing the quit. Nothing off macOS.
void ignorePersistentUIState();
} // namespace SelfDestruct
