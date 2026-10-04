#include "TestApp.h"

namespace eacp::WebView::Test
{

Vector<std::function<void()>>& Detail::restartRegistry()
{
    return Singleton::get<Vector<std::function<void()>>>();
}

void Detail::runAllRestarts()
{
    for (auto& cb: restartRegistry())
        cb();
}

TestProxy test(std::string_view name)
{
    return {nano::test(name)};
}

} // namespace eacp::WebView::Test
