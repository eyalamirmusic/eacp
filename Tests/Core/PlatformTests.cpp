#include "Common.h"

using namespace nano;

auto tArchitectureName = test("Platform/architectureName names a known CPU") = []
{
    const auto name = eacp::Platform::architectureName();
    check(name == "arm64" || name == "x64");
};
