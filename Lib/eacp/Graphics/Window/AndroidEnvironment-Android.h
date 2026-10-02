#pragma once

struct ANativeActivity;

namespace eacp::Graphics
{
// An app `am start` launches inherits no environment of its own, so this sets
// it from the system property debug.<package>.env ("K=V K=V"), which persists
// across launches, and then from the launch intent's string extras
// (`am start ... --es K V`), which win where both name a variable.
void importAndroidEnvironment(ANativeActivity* activity);
} // namespace eacp::Graphics
