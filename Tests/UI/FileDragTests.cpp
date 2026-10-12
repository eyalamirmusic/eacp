#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
eacp::Graphics::FileDragOptions countingOptions(int& finished)
{
    auto options = eacp::Graphics::FileDragOptions {};
    options.onFinished = [&finished](eacp::Graphics::FileDragResult) { ++finished; };
    return options;
}
} // namespace

auto tNoHostNoDrag = test("FileDrag/aComponentWithoutAHostCannotStartADrag") = []
{
    auto component = Component {};
    auto finished = 0;

    check(!component.startFileDrag({"/tmp/pattern.mid"}, countingOptions(finished)));
    check(finished == 0);
};

auto tHostOutsideAWindow = test("FileDrag/aHostOutsideAWindowCannotStartADrag") = []
{
    auto host = ComponentHost {};
    auto root = Component {};
    auto child = Component {};

    host.setBounds({0.f, 0.f, 200.f, 100.f});
    host.setRootComponent(root);
    root.addAndMakeVisible(child);
    child.setBounds({10.f, 10.f, 50.f, 50.f});

    auto finished = 0;

    check(!child.startFileDrag({"/tmp/pattern.mid"}, countingOptions(finished)));
    check(!child.startFileDrag({}, countingOptions(finished)));
    check(finished == 0);
};
