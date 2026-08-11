#pragma once

// Pure logic around the untestable native modal, plus the dialog seams tests
// substitute a fake for.

#include "App.h"

namespace eacp::Apps::detail
{
// {"wav", "mp3"} -> L"*.wav;*.mp3".
std::wstring buildFilterPattern(const Vector<std::string>& extensions);

// null/empty (cancelled) -> nullopt; else the wide path as UTF-8.
std::optional<std::string> shellResultToPath(const wchar_t* pickedWidePath);

using ShellOpenDialog = std::function<std::optional<std::wstring>(
    bool pickFolders, const FilePickerOptions& options)>;

std::optional<std::string> chooseWithDialog(bool pickFolders,
                                            const FilePickerOptions& options,
                                            const ShellOpenDialog& dialog);

using ShellSaveDialog =
    std::function<std::optional<std::wstring>(const FileSaveOptions& options)>;

std::optional<std::string> chooseSaveWithDialog(const FileSaveOptions& options,
                                                const ShellSaveDialog& dialog);
} // namespace eacp::Apps::detail
