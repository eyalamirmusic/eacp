#pragma once

/** @file
    Batched 2D textured quads over eacp-gpu: SpriteRenderer draws textures,
    sub-rects, arbitrary parallelograms, NV12 video frames, rects and lines
    inside a RenderPass, in float logical units. Built where EACP_HAS_GPU is.

    Quads that share a texture and sampling are queued and drawn as one
    instanced draw; order is never changed. The pass draws what is queued when
    it ends, so begin(pass) is the only protocol. Call flush() before changing
    pass state by hand, and prefer the renderer's own setScissorRect, which
    takes render-target pixels, not points. A texture is not retained: it must
    outlive the pass it was drawn in. fitRect places a video-shaped image
    inside an area (Stretch, Contain, Cover).
*/

#include "Fit.h"
#include "SpriteRenderer.h"
