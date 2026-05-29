#pragma once

#include <Miro/Miro.h>

#include <string>
#include <vector>

namespace eacp::WebView
{

// A single file the page can hand to the native drag-out. `path` is the
// absolute on-disk path the OS copies on drop; `name` is the display label
// (defaults to the basename, but the app may override it).
struct DraggableFile
{
    std::string path;
    std::string name;

    MIRO_REFLECT(path, name)
};

// Payload of the built-in `armFileDrag` bridge command. Serializable via Miro,
// so the page sends `{ files: [{ path, name }, ...] }` and the bridge
// deserializes it straight into this type -- no hand-rolled JSON on either
// side. Arming with multiple files starts a single multi-file drag session.
struct DraggableFileList
{
    std::vector<DraggableFile> files;

    MIRO_REFLECT(files)
};

} // namespace eacp::WebView
