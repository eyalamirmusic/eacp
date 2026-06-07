#pragma once

#include <eacp/Core/Utils/Common.h>
#include <eacp/Graphics/Primitives/Primitives.h>

#include "RenderPass.h"

namespace eacp::GPU
{
class Device;

struct RenderPassDescriptor
{
    Graphics::Color clearColor = Graphics::Color::black();
    bool clear = true;
};

// One renderable frame: owns the drawable being rendered to plus its command
// buffer. Presents the drawable and commits the command buffer on destruction.
// Created by GPUView each tick and handed to GPUView::render. When msaaTexture
// is non-null the pass renders into it and resolves into the drawable.
class Frame
{
public:
    Frame(Device& device, void* drawable, void* msaaTexture, void* depthTexture);
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
