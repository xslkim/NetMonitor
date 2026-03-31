#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <cstdint>
#include "core/Types.h"

namespace Dialogs {

// Show a dialog to set bandwidth limit (bytes/sec). Returns 0 if cancelled.
uint64_t showLimitDialog(HWND parent, const std::wstring& processName, Direction dir);

// Show a dialog to add an alert policy. Returns false if cancelled.
bool showAlertDialog(HWND parent, const std::wstring& processName, uint32_t pid, AlertPolicy& outPolicy);

// Show an alert notification
void showAlertNotification(HWND parent, const AlertPolicy& policy);

} // namespace Dialogs
