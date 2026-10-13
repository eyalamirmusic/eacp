#include "App.h"

// The async choosers where the platform has no modeless panel of its own: the
// blocking chooser, run from the event loop. macOS has its own in App-macOS.mm.

namespace eacp::Apps
{
void chooseFileAsync(const FilePickerOptions& options, FileChosenCallback onChosen)
{
    Detail::runChooserLater([options] { return chooseFile(options); },
                            std::move(onChosen));
}

void chooseSaveFileAsync(const FileSaveOptions& options, FileChosenCallback onChosen)
{
    Detail::runChooserLater([options] { return chooseSaveFile(options); },
                            std::move(onChosen));
}

void chooseDirectoryAsync(const FilePickerOptions&, FileChosenCallback onChosen)
{
    Detail::runChooserLater([] { return chooseDirectory(); }, std::move(onChosen));
}
} // namespace eacp::Apps
