#pragma once

// The stand-in for a hosted plugin's editor: a native view of a toolkit that
// is not ours, parented into a NativeChildSurface and drawing itself. It has a
// real platform button in it, and the button prints when it fires — which is
// how a press that reached the plugin, or was swallowed by the popup sitting
// over it, can be told apart from the terminal.
//
// `nativeParentHandle` is NativeChildSurface::getNativeParentHandle(): an
// NSView* on macOS, a child HWND on Windows.
void addForeignContent(void* nativeParentHandle);
