#include "Alert.h"

// Linux and Android: no dialog of eacp's own yet, so every alert is the
// headless one.

namespace eacp::Apps
{
void Detail::showNativeAlert(const AlertOptions& options, AlertCallback onDismissed)
{
    answerUnshownAlert(options, onDismissed);
}
} // namespace eacp::Apps
