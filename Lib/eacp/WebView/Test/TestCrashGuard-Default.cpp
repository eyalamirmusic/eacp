#include "TestCrashGuard.h"

namespace eacp::WebView::Test
{

void installShutdownCrashGuard()
{
    // No teardown crash guard needed outside Windows.
}

void markTestShutdown(int) {}

} // namespace eacp::WebView::Test
