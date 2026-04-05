#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include "core/Types.h"

namespace Dialogs {

// Register window classes (call once at startup)
void registerClasses(HINSTANCE hInstance);

// Show dialog to set bandwidth limit. Returns bytes/sec, 0 = cancel, UINT64_MAX = remove limit.
uint64_t showLimitDialog(HWND parent, const std::wstring& processName,
                          Direction dir, uint64_t currentLimitBytesPerSec);

// Show dialog to configure alert policy. Returns false if cancelled.
bool showAlertDialog(HWND parent, const std::wstring& processName,
                     uint32_t pid, AlertPolicy& outPolicy);

// Show list of all alert policies.
void showAlertListDialog(HWND parent, const std::vector<AlertPolicy>& policies);

// Show alert notification (non-blocking via MessageBox on separate thread).
void showAlertNotification(HWND parent, const AlertPolicy& policy);

// Show traffic history log viewer (modal dialog with ListView).
void showLogViewerDialog(HWND parent);

} // namespace Dialogs
