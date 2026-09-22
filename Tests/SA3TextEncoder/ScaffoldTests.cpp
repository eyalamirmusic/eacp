#include <eacp/SA3TextEncoder/SA3TextEncoder.h>

#include <NanoTest/NanoTest.h>

using namespace nano;

auto tSA3TextEncoderScaffolded = test("SA3TextEncoder/isScaffolded") = []
{
    check(eacp::SA3TextEncoder::isScaffolded());
};
