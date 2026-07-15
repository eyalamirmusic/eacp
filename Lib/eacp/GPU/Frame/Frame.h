#pragma once

#include "RenderPass.h"

namespace eacp::GPU
{
class Device;

struct RenderPassDescriptor
{
    Graphics::Color clearColor = Graphics::Color::black();
    bool clear = true;
};

// Off-screen render target for snapshots: a colour texture the app owns instead
// of a swapchain drawable, resolved from msaaTexture when multisampling. A Frame
// built from one renders into it and, on destruction, commits and waits so the
// texture is ready to read back -- it never presents.
struct OffscreenTarget
{
    void* colorTexture = nullptr;
    void* msaaTexture = nullptr;
    void* depthTexture = nullptr;
};

// One renderable frame: owns the drawable being rendered to plus its command
// buffer. Presents the drawable and commits the command buffer on destruction.
// Created by GPUView each tick and handed to GPUView::render. When msaaTexture
// is non-null the pass renders into it and resolves into the drawable.
class Frame
{
public:
    Frame(Device& device, void* drawable, void* msaaTexture, void* depthTexture);
    Frame(Device& device, const OffscreenTarget& target);
    ~Frame();

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    RenderPass beginPass(const RenderPassDescriptor& descriptor = {});

    bool isValid() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
