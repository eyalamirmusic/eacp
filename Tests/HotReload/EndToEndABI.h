#pragma once

// Defined in the app's EndToEndApp.cpp and called by the module, which reaches
// it only through the app's export list.
int hotReloadEndToEndBase();

using EndToEndAnswer = int();
