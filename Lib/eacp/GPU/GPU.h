#pragma once

/** @file
    GPU rendering and compute over Metal (Apple), D3D12 (Windows) and Vulkan
    (Linux and Android), gated by EACP_HAS_GPU. A Device owns the queue and the
    resources (Buffer, Texture, Sampler); a Frame is one command buffer, opened
    by GPUView::render or off screen, in which RenderPass and ComputePass record
    work against RenderPipeline and ComputePipeline. Shaders are written in C++
    with the EDSL under Codegen/ (ShaderProgram, ComputeProgram), which emits
    MSL, HLSL or GLSL into a ShaderSource; eacp_add_precompiled_shaders compiles
    them at build time instead. eacp-gpu-codegen and eacp-cpu-compute (which
    runs compute kernels on the CPU, see CpuCompute/CpuCompute.h, not included
    here) build on every platform with no device.
    Rules: a Device is single-threaded, so record and submit from the thread
    that owns it, normally the main thread. Inside a component tree, draw with
    UI::GPUComponent rather than a child GPUView. Lib/eacp/GPU/README.md is the
    full account.
*/

#include "Buffer/StreamingBuffers.h"
#include "Codegen/Codegen.h"
#include "Frame/Frame.h"
#include "Shader/Dxc.h"
#include "Texture/MipChain.h"
#include "View/GPUView.h"
#include <eacp/Graphics/Graphics.h>
