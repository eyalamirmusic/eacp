#include "Common.h"

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

auto tOptionsDefault = test("FileDrag/optionsDefaultToACopyWithTheFileIcons") = []
{
    auto options = FileDragOptions {};

    check(!options.canMove);
    check(!options.image.isValid());
    check(options.imageScale == 1.f);
    check(!options.onFinished);
};

auto tNoPathsNoDrag = test("FileDrag/nothingToDragStartsNothing") = []
{
    auto view = View {};
    auto finished = 0;

    auto options = FileDragOptions {};
    options.onFinished = [&](FileDragResult) { ++finished; };

    check(!view.startFileDrag({}, options));
    check(finished == 0);
};

auto tNoWindowNoDrag = test("FileDrag/aViewOutsideAWindowCannotStartADrag") = []
{
    auto view = View {};
    view.setBounds({0.f, 0.f, 100.f, 100.f});
    auto finished = 0;

    auto options = FileDragOptions {};
    options.onFinished = [&](FileDragResult) { ++finished; };

    check(!view.startFileDrag({"/tmp/pattern.mid"}, options));
    check(finished == 0);
};
