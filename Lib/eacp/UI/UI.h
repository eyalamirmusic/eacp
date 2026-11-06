#pragma once

/** @file
    A component tree drawn on the GPU. A Component tree lives inside one
    ComponentHost, which is a single GPU::GPUView: one swapchain, one display
    link and one pass per frame however many widgets, paths, text runs, images
    and layers the tree holds. Entry points: Component (paint with Graphics),
    ComponentHost (setRootComponent), the stock widgets in Widgets/, and
    GPUComponent for custom GPU rendering inside the tree.

    Built where EACP_HAS_TEXT is (Apple, Windows, Linux, Android).
    Components are main-thread only. A component does not own its children:
    addAndMakeVisible links a component the caller keeps alive. Use SafePointer
    in any callback that may outlive its component. GPU rendering inside a tree
    goes through GPUComponent, never a native child GPUView, which draws above
    the whole tree unclipped. Lib/eacp/UI/README.md is the long account.
*/

#include "Common.h"
#include "Component/Component.h"
#include "Component/GPUComponent.h"
#include "Component/KeyEvent.h"
#include "Component/MouseEvent.h"
#include "Component/SafePointer.h"
#include "DragAndDrop/DragAndDrop.h"
#include "DragAndDrop/FileDropTarget.h"
#include "Graphics/Graphics.h"
#include "Host/ComponentHost.h"
#include "Render/ClipMask.h"
#include "Render/CoverageAtlas.h"
#include "Render/DrawList.h"
#include "Render/DrawPlayer.h"
#include "Render/Gradient.h"
#include "Render/GradientRamps.h"
#include "Render/ImageBatch.h"
#include "Render/ImageCache.h"
#include "Render/ImageTexture.h"
#include "Render/Layer.h"
#include "Render/LayerRenderer.h"
#include "Render/MaskCache.h"
#include "Render/MeshBatch.h"
#include "Render/PathShape.h"
#include "Render/ShapeBatch.h"
#include "Widgets/ComboBox.h"
#include "Widgets/ListBox.h"
#include "Widgets/PopupMenu.h"
#include "Widgets/Tabs.h"
#include "Widgets/TouchScroller.h"
#include "Widgets/Viewport.h"
#include "Widgets/Widgets.h"
