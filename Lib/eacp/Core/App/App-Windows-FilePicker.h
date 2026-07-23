#pragma once

// The native picker (IFileOpenDialog::Show) is an untestable modal; these pure
// helpers factor out its surrounding logic so tests can drive it with a fake.

#include "App.h"

namespace eacp::Apps::detail
{
// {"wav", "mp3"} -> L"*.wav;*.mp3".
std::wstring buildFilterPattern(const Vector<std::string>& extensions);

// null/empty (cancelled) -> nullopt; else the wide path as UTF-8.
std::optional<std::string> shellResultToPath(const wchar_t* pickedWidePath);

// The modal itself — the injection seam; tests pass a fake.
using ShellOpenDialog = std::function<std::optional<std::wstring>(
    bool pickFolders, const FilePickerOptions& options)>;

// Shared core of chooseFile()/chooseDirectory(): run dialog, convert result.
std::optional<std::string> chooseWithDialog(bool pickFolders,
                                            const FilePickerOptions& options,
                                            const ShellOpenDialog& dialog);

// The save modal's seam, same idea as ShellOpenDialog above.
using ShellSaveDialog =
    std::function<std::optional<std::wstring>(const FileSaveOptions& options)>;

// Shared core of chooseSaveFile().
std::optional<std::string> chooseSaveWithDialog(const FileSaveOptions& options,
                                                const ShellSaveDialog& dialog);
} // namespace eacp::Apps::detail
