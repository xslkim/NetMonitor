#include "gui/Dialogs.h"
#include <commctrl.h>
#include <sstream>

#pragma comment(lib, "comctl32.lib")

// Simple input dialog using a modal message loop with a custom window
struct LimitDialogData {
    std::wstring processName;
    Direction direction;
    uint64_t result;
    bool confirmed;
};

static INT_PTR CALLBACK LimitDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    static LimitDialogData* data = nullptr;

    switch (msg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<LimitDialogData*>(lParam);
        std::wstring title = L"设置" + data->processName +
            (data->direction == Direction::Upload ? L" 上行限速" : L" 下行限速");
        SetWindowTextW(hDlg, title.c_str());

        // Create label
        CreateWindowW(L"STATIC", L"限速 (KB/s, 0=取消限制):",
            WS_CHILD | WS_VISIBLE, 10, 15, 220, 20, hDlg, nullptr,
            GetModuleHandle(nullptr), nullptr);

        // Create edit
        HWND hEdit = CreateWindowW(L"EDIT", L"0",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
            240, 12, 100, 22, hDlg, (HMENU)101,
            GetModuleHandle(nullptr), nullptr);
        SetFocus(hEdit);

        // OK button
        CreateWindowW(L"BUTTON", L"确定",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            100, 50, 80, 28, hDlg, (HMENU)IDOK,
            GetModuleHandle(nullptr), nullptr);

        // Cancel button
        CreateWindowW(L"BUTTON", L"取消",
            WS_CHILD | WS_VISIBLE,
            200, 50, 80, 28, hDlg, (HMENU)IDCANCEL,
            GetModuleHandle(nullptr), nullptr);

        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            wchar_t buf[64] = {};
            GetDlgItemTextW(hDlg, 101, buf, 64);
            uint64_t kbps = _wtoi64(buf);
            data->result = kbps * 1024; // convert KB/s to bytes/s
            data->confirmed = true;
            EndDialog(hDlg, IDOK);
        } else if (LOWORD(wParam) == IDCANCEL) {
            data->confirmed = false;
            EndDialog(hDlg, IDCANCEL);
        }
        return TRUE;
    case WM_CLOSE:
        data->confirmed = false;
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK AlertDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    static AlertPolicy* policyOut = nullptr;

    switch (msg) {
    case WM_INITDIALOG: {
        policyOut = reinterpret_cast<AlertPolicy*>(lParam);
        std::wstring title = L"添加报警策略 - " + policyOut->processName;
        SetWindowTextW(hDlg, title.c_str());

        int y = 15;
        auto makeLabel = [&](const wchar_t* text) {
            CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE,
                10, y, 200, 20, hDlg, nullptr, GetModuleHandle(nullptr), nullptr);
        };
        auto makeEdit = [&](int id, const wchar_t* def) {
            HWND h = CreateWindowW(L"EDIT", def,
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                220, y - 3, 100, 22, hDlg, (HMENU)(INT_PTR)id,
                GetModuleHandle(nullptr), nullptr);
            return h;
        };

        makeLabel(L"监控时间窗口 (分钟):"); makeEdit(201, L"10"); y += 30;
        makeLabel(L"流量阈值 (MB):"); makeEdit(202, L"500"); y += 30;
        makeLabel(L"触发后限速 (KB/s):"); makeEdit(203, L"10"); y += 30;

        // Direction combo
        makeLabel(L"方向:");
        HWND hCombo = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            220, y - 3, 100, 100, hDlg, (HMENU)204,
            GetModuleHandle(nullptr), nullptr);
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"下行");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"上行");
        SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
        y += 40;

        CreateWindowW(L"BUTTON", L"确定",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            100, y, 80, 28, hDlg, (HMENU)IDOK,
            GetModuleHandle(nullptr), nullptr);
        CreateWindowW(L"BUTTON", L"取消",
            WS_CHILD | WS_VISIBLE,
            200, y, 80, 28, hDlg, (HMENU)IDCANCEL,
            GetModuleHandle(nullptr), nullptr);

        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK && policyOut) {
            wchar_t buf[64];

            GetDlgItemTextW(hDlg, 201, buf, 64);
            policyOut->windowSeconds = _wtoi(buf) * 60;

            GetDlgItemTextW(hDlg, 202, buf, 64);
            policyOut->thresholdBytes = _wtoi64(buf) * 1024 * 1024;

            GetDlgItemTextW(hDlg, 203, buf, 64);
            policyOut->limitBytesPerSec = _wtoi64(buf) * 1024;

            LRESULT sel = SendDlgItemMessageW(hDlg, 204, CB_GETCURSEL, 0, 0);
            policyOut->direction = (sel == 1) ? Direction::Upload : Direction::Download;

            EndDialog(hDlg, IDOK);
        } else if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
        }
        return TRUE;
    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

namespace Dialogs {

uint64_t showLimitDialog(HWND parent, const std::wstring& processName, Direction dir) {
    // Register and create a dialog dynamically
    // Using a DLGTEMPLATE in memory
    alignas(4) BYTE dlgBuf[512] = {};
    DLGTEMPLATE* dlg = reinterpret_cast<DLGTEMPLATE*>(dlgBuf);
    dlg->style = DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER;
    dlg->cx = 200;
    dlg->cy = 60;

    LimitDialogData data;
    data.processName = processName;
    data.direction = dir;
    data.result = 0;
    data.confirmed = false;

    DialogBoxIndirectParamW(GetModuleHandle(nullptr), dlg, parent,
                            LimitDlgProc, reinterpret_cast<LPARAM>(&data));

    return data.confirmed ? data.result : 0;
}

bool showAlertDialog(HWND parent, const std::wstring& processName, uint32_t pid, AlertPolicy& outPolicy) {
    alignas(4) BYTE dlgBuf[512] = {};
    DLGTEMPLATE* dlg = reinterpret_cast<DLGTEMPLATE*>(dlgBuf);
    dlg->style = DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER;
    dlg->cx = 200;
    dlg->cy = 110;

    outPolicy.pid = pid;
    outPolicy.processName = processName;

    INT_PTR ret = DialogBoxIndirectParamW(GetModuleHandle(nullptr), dlg, parent,
                                           AlertDlgProc, reinterpret_cast<LPARAM>(&outPolicy));
    return ret == IDOK;
}

void showAlertNotification(HWND parent, const AlertPolicy& policy) {
    std::wstringstream ss;
    ss << L"⚠ 报警触发!\n\n"
       << L"进程: " << policy.processName << L" (PID: " << policy.pid << L")\n"
       << (policy.direction == Direction::Download ? L"下行" : L"上行")
       << L"流量在 " << (policy.windowSeconds / 60) << L" 分钟内超过 "
       << (policy.thresholdBytes / 1024 / 1024) << L" MB\n\n"
       << L"已自动限速为 " << (policy.limitBytesPerSec / 1024) << L" KB/s";

    MessageBoxW(parent, ss.str().c_str(), L"NetMonitor 报警", MB_OK | MB_ICONWARNING);
}

} // namespace Dialogs
