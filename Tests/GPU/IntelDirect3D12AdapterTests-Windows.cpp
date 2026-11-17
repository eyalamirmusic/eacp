#include "Common.h"

#include <eacp/GPU/Windows/D3D12Context.h>

using namespace nano;
using namespace eacp::GPU;

namespace IntelDirect3D12AdapterTests
{
constexpr UINT intelVendorId = 0x8086;
constexpr UINT nvidiaVendorId = 0x10DE;
constexpr UINT amdVendorId = 0x1002;

constexpr UINT hdGraphics4600 = 0x0412;
constexpr UINT ivyBridgeHdGraphics4000 = 0x0166;
constexpr UINT haswellUltHdGraphics5000 = 0x0A26;
constexpr UINT haswellSoftwareDevelopmentVehicle = 0x0C02;
constexpr UINT crystalWellIrisPro5200 = 0x0D22;
constexpr UINT bayTrailHdGraphics = 0x0F31;

constexpr UINT irisXe = 0x9A49;
constexpr UINT uhdGraphics620 = 0x3EA0;
constexpr UINT skylakeHdGraphics530 = 0x1912;

auto tRefusesGen7 =
    test("IntelDirect3D12Adapter/refusesEveryIntelGeneration7Family") = []
{
    for (auto deviceId: {hdGraphics4600,
                         ivyBridgeHdGraphics4000,
                         haswellUltHdGraphics5000,
                         haswellSoftwareDevelopmentVehicle,
                         crystalWellIrisPro5200,
                         bayTrailHdGraphics})
    {
        check(isIntelAdapterWhoseDirect3D12LosesItsDevice(intelVendorId, deviceId));
    }
};

auto tAllowsNewerIntel = test("IntelDirect3D12Adapter/allowsNewerIntelGraphics") = []
{
    for (auto deviceId: {irisXe, uhdGraphics620, skylakeHdGraphics530})
        check(!isIntelAdapterWhoseDirect3D12LosesItsDevice(intelVendorId, deviceId));
};

auto tAllowsOtherVendors =
    test("IntelDirect3D12Adapter/allowsOtherVendorsWithAnyDeviceId") = []
{
    for (auto vendorId: {nvidiaVendorId, amdVendorId})
    {
        for (auto deviceId: {hdGraphics4600, bayTrailHdGraphics, irisXe})
            check(!isIntelAdapterWhoseDirect3D12LosesItsDevice(vendorId, deviceId));
    }
};
} // namespace IntelDirect3D12AdapterTests
