#include <eacp/SA3DiT/SA3DiT.h>

#include <NanoTest/NanoTest.h>

using namespace nano;

auto tSA3DiTScaffolded = test("SA3DiT/isScaffolded") = []
{
    check(eacp::SA3DiT::isScaffolded());
};
