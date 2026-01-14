#pragma once

namespace eacp::ObjC
{
class AutoReleasePool
{
public:
    AutoReleasePool();
    ~AutoReleasePool();

    AutoReleasePool(const AutoReleasePool&) = delete;

private:
    void* context {nullptr};
};
} // namespace eacp::ObjC
