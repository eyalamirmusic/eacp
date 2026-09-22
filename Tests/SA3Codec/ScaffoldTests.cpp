#include <eacp/SA3Codec/SA3Codec.h>

#include <NanoTest/NanoTest.h>

using namespace nano;

auto tSA3CodecScaffolded = test("SA3Codec/isScaffolded") = []
{
    check(eacp::SA3Codec::isScaffolded());
};
