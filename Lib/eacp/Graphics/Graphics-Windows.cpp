// Windows-specific implementations for eacp-graphics using Direct2D

// Direct2D/DirectWrite factory initialization - must be first
#include "Graphics/D2DFactory-Windows.cpp"

// Primitives
#include "Primitives/Path-Windows.cpp"
#include "Primitives/Font-Windows.cpp"
#include "Primitives/TextMetrics-Windows.cpp"

// Graphics
#include "Graphics/Layer-Windows.cpp"
#include "Graphics/ShapeLayer-Windows.cpp"
#include "Graphics/TextLayer-Windows.cpp"
#include "Graphics/Keyboard-Windows.cpp"
#include "Graphics/View-Windows.cpp"
#include "Graphics/Window-Windows.cpp"

// Helpers
#include "Helpers/DisplayLink-Windows.cpp"
