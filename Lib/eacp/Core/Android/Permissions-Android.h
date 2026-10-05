#pragma once

#include <functional>

// Runtime permissions, named in full, such as "android.permission.CAMERA". The
// permission must also be in the manifest (eacp_add_app's PERMISSIONS), or the
// system denies it without asking.
namespace eacp::Android
{
bool hasPermission(const char* permission);

// Answers on the main thread, once. A NativeActivity is never told the dialog's
// result, so the answer is read back when the activity resumes after it.
void requestPermission(const char* permission,
                       const std::function<void(bool)>& onResult);

namespace Detail
{
void permissionsActivityPaused();
void permissionsActivityResumed();
} // namespace Detail
} // namespace eacp::Android
