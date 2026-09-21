#include "TestCrashGuard.h"

namespace eacp::WebView::Test
{

// No teardown crash guard needed outside Windows.
void installShutdownCrashGuard() {}

void markTestShutdown(int) {}

} // namespace eacp::WebView::Test
