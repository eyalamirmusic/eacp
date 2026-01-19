// Primitives (shared - CoreGraphics/CoreText work on both platforms)
#include "Primitives/GraphicUtils.mm"
#include "Primitives/Font.mm"
#include "Primitives/Path.mm"
#include "Primitives/TextMetrics.mm"

// Graphics (shared - CALayer works on both platforms)
#include "Graphics/GraphicsContextImpl.mm"
#include "Graphics/Layer.mm"
#include "Graphics/ShapeLayer.mm"
#include "Graphics/TextLayer.mm"

// View and Window shared logic
#include "Graphics/View.mm"
#include "Graphics/Window.mm"

// TextInput and WebView (WKWebView works on both platforms)
#include "Graphics/TextInput.mm"
#include "Graphics/WebView.mm"
