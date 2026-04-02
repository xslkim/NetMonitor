#include "gui/Dialogs.h"
#include <commctrl.h>
#include <sstream>
#include <vector>
#include <thread>

#pragma comment(lib, "comctl32.lib")

static const wchar_t* DLG_CLASS = L"NetMonitorModalDlg";
static const wchar_t* LIST_CLASS = L"NetMonitorListDlg";

// ─── Modal window helper ──────────────────────────────────────────────────────

struct ModalCtx {
    INT_PTR result = 0;
    bool closed = false;
};

static void endModal(HWND hwnd, INT_PTR result) {
    auto* ctx = reinterpret_cast<ModalCtx*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (ctx) {
        ctx->result = result;
        ctx->closed = true;
    }
    DestroyWindow(hwnd);
}

static INT_PTR runModal(HWND hwnd, HWND parent) {
    ModalCtx ctx;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&ctx));

    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (!ctx.closed && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    return ctx.result;
}

static HWND createCenteredWindow(HWND parent, const wchar_t* cls,
                                  const wchar_t* title, int w, int h) {
    RECT pr{};
    if (parent) GetWindowRect(parent, &pr);
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &pr, 0);

    int x = pr.left + (pr.right - pr.left - w) / 2;
    int y = pr.top  + (pr.bottom - pr.top  - h) / 2;

    return CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        cls, title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, w, h,
        parent, nullptr, GetModuleHandle(nullptr), nullptr);
}

static HWND makeLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowW(L"STATIC", text,
        WS_CHILD | WS_VISIBLE,
        x, y, w, h, parent, nullptr, GetModuleHandle(nullptr), nullptr);
}

static HWND makeEdit(HWND parent, const wchar_t* def, int id, int x, int y, int w, int h,
                      bool numbersOnly = true) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP;
    if (numbersOnly) style |= ES_NUMBER;
    HWND hwndEdit = CreateWindowW(L"EDIT", def, style,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, GetModuleHandle(nullptr), nullptr);
    return hwndEdit;
}

static HWND makeButton(HWND parent, const wchar_t* text, int id,
                        int x, int y, int w, int h, bool isDefault = false) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
    if (isDefault) style |= BS_DEFPUSHBUTTON;
    return CreateWindowW(L"BUTTON", text, style,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, GetModuleHandle(nullptr), nullptr);
}

static HWND makeCombo(HWND parent, int id, int x, int y, int w) {
    return CreateWindowW(L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        x, y, w, 120, parent, (HMENU)(INT_PTR)id, GetModuleHandle(nullptr), nullptr);
}

// ─── Limit dialog ─────────────────────────────────────────────────────────────

struct LimitData {
    std::wstring processName;
    Direction    direction;
    uint64_t     currentLimit;
    uint64_t     result;
};

static LRESULT CALLBACK LimitWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static LimitData* data = nullptr;
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        data = reinterpret_cast<LimitData*>(cs->lpCreateParams);

        const wchar_t* dirStr = (data->direction == Direction::Upload) ? L"上行" : L"下行";
        uint64_t curKb = data->currentLimit / 1024;

        makeLabel(hwnd, (std::wstring(dirStr) + L" 限速 (KB/s):").c_str(),
                  15, 22, 160, 22);
        makeEdit(hwnd, curKb > 0 ? std::to_wstring(curKb).c_str() : L"",
                 101, 180, 19, 90, 26);
        makeLabel(hwnd, L"填 0 或留空 = 取消限制", 15, 58, 250, 20);
        makeButton(hwnd, L"确定", IDOK,     165, 90, 90, 30, true);
        makeButton(hwnd, L"取消", IDCANCEL, 265, 90, 90, 30);

        // Focus edit
        PostMessageW(hwnd, WM_NEXTDLGCTL,
                     (WPARAM)GetDlgItem(hwnd, 101), TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK && data) {
            wchar_t buf[32] = {};
            GetDlgItemTextW(hwnd, 101, buf, 32);
            uint64_t kb = _wtoi64(buf);
            data->result = kb * 1024;
            endModal(hwnd, IDOK);
        } else if (LOWORD(wp) == IDCANCEL) {
            endModal(hwnd, IDCANCEL);
        }
        return 0;
    case WM_CLOSE:
        endModal(hwnd, IDCANCEL);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ─── Alert dialog ─────────────────────────────────────────────────────────────

struct AlertData {
    std::wstring processName;
    uint32_t     pid;
    AlertPolicy* out;
};

static LRESULT CALLBACK AlertWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static AlertData* data = nullptr;
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        data = reinterpret_cast<AlertData*>(cs->lpCreateParams);

        int col1 = 15, col2 = 195, ew = 110;
        int y = 20;

        makeLabel(hwnd, L"监控方向:", col1, y + 3, 170, 22);
        HWND hCombo = makeCombo(hwnd, 201, col2, y, ew);
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"下行 (Download)");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"上行 (Upload)");
        SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
        y += 38;

        makeLabel(hwnd, L"时间窗口 (分钟):", col1, y + 3, 170, 22);
        makeEdit(hwnd, L"10", 202, col2, y, ew, 26);
        y += 38;

        makeLabel(hwnd, L"流量阈值 (MB):", col1, y + 3, 170, 22);
        makeEdit(hwnd, L"500", 203, col2, y, ew, 26);
        y += 38;

        makeLabel(hwnd, L"触发后限速 (KB/s):", col1, y + 3, 170, 22);
        makeEdit(hwnd, L"10", 204, col2, y, ew, 26);
        makeLabel(hwnd, L"(填 0 = 不限速，仅报警)", col2 + ew + 5, y + 3, 150, 22);
        y += 50;

        makeButton(hwnd, L"确定", IDOK,     160, y, 90, 30, true);
        makeButton(hwnd, L"取消", IDCANCEL, 260, y, 90, 30);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK && data && data->out) {
            wchar_t buf[64] = {};

            LRESULT sel = SendDlgItemMessageW(hwnd, 201, CB_GETCURSEL, 0, 0);
            data->out->direction = (sel == 1) ? Direction::Upload : Direction::Download;

            GetDlgItemTextW(hwnd, 202, buf, 64);
            int minutes = _wtoi(buf);
            data->out->windowSeconds = (minutes <= 0 ? 10 : minutes) * 60;

            GetDlgItemTextW(hwnd, 203, buf, 64);
            uint64_t mb = _wtoi64(buf);
            data->out->thresholdBytes = (mb == 0 ? 500ULL : mb) * 1024 * 1024;

            GetDlgItemTextW(hwnd, 204, buf, 64);
            data->out->limitBytesPerSec = _wtoi64(buf) * 1024;

            endModal(hwnd, IDOK);
        } else if (LOWORD(wp) == IDCANCEL) {
            endModal(hwnd, IDCANCEL);
        }
        return 0;
    case WM_CLOSE:
        endModal(hwnd, IDCANCEL);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ─── Alert list dialog ────────────────────────────────────────────────────────

static LRESULT CALLBACK AlertListWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static HWND hList = nullptr;
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        auto* policies = reinterpret_cast<std::vector<AlertPolicy>*>(cs->lpCreateParams);

        RECT rc;
        GetClientRect(hwnd, &rc);
        int bh = 36;

        hList = CreateWindowExW(0, WC_LISTVIEWW, nullptr,
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, rc.right, rc.bottom - bh, hwnd, nullptr,
            GetModuleHandle(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(hList,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        // Columns
        struct { const wchar_t* name; int w; } cols[] = {
            {L"进程名",     130}, {L"方向",  60}, {L"时间窗口", 80},
            {L"阈值",       90},  {L"限速",  90}, {L"状态",     80}
        };
        for (int i = 0; i < 6; i++) {
            LVCOLUMNW col = {};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.pszText = const_cast<LPWSTR>(cols[i].name);
            col.cx = cols[i].w;
            col.iSubItem = i;
            ListView_InsertColumn(hList, i, &col);
        }

        // Rows
        for (int r = 0; r < (int)policies->size(); r++) {
            const auto& p = (*policies)[r];
            LVITEMW item = {};
            item.mask = LVIF_TEXT;
            item.iItem = r;
            item.pszText = const_cast<LPWSTR>(p.processName.c_str());
            ListView_InsertItem(hList, &item);

            auto setCol = [&](int c, std::wstring t) {
                ListView_SetItemText(hList, r, c, const_cast<LPWSTR>(t.c_str()));
            };
            setCol(1, p.direction == Direction::Download ? L"下行" : L"上行");
            setCol(2, std::to_wstring(p.windowSeconds / 60) + L" 分钟");
            setCol(3, std::to_wstring(p.thresholdBytes / 1024 / 1024) + L" MB");
            if (p.limitBytesPerSec > 0)
                setCol(4, std::to_wstring(p.limitBytesPerSec / 1024) + L" KB/s");
            else
                setCol(4, L"仅报警");
            setCol(5, p.triggered ? L"已触发" : L"监控中");
        }

        makeButton(hwnd, L"关闭", IDOK, rc.right / 2 - 45, rc.bottom - 30, 90, 26, true);
        return 0;
    }
    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        if (hList) MoveWindow(hList, 0, 0, w, h - 36, TRUE);
        HWND hBtn = GetDlgItem(hwnd, IDOK);
        if (hBtn) MoveWindow(hBtn, w / 2 - 45, h - 30, 90, 26, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) endModal(hwnd, IDOK);
        return 0;
    case WM_CLOSE:
        endModal(hwnd, IDOK);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ─── Public API ───────────────────────────────────────────────────────────────

namespace Dialogs {

void registerClasses(HINSTANCE hInstance) {
    auto reg = [&](const wchar_t* name, WNDPROC proc) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = proc;
        wc.hInstance     = hInstance;
        wc.lpszClassName = name;
        wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
    };
    reg(DLG_CLASS,  LimitWndProc);
    reg(LIST_CLASS, AlertListWndProc);

    // AlertWndProc reuses DLG_CLASS but needs its own — register separately
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = AlertWndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = L"NetMonitorAlertDlg";
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
}

uint64_t showLimitDialog(HWND parent, const std::wstring& processName,
                          Direction dir, uint64_t currentLimit) {
    std::wstring title = processName + (dir == Direction::Upload ? L" - 上行限速" : L" - 下行限速");

    LimitData data{processName, dir, currentLimit, 0};

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, DLG_CLASS, title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, 380, 160, parent, nullptr, GetModuleHandle(nullptr), &data);
    if (!hwnd) return UINT64_MAX; // cancelled

    // Center on parent
    RECT pr{}, wr{};
    GetWindowRect(parent, &pr);
    GetWindowRect(hwnd, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    SetWindowPos(hwnd, nullptr,
                 pr.left + (pr.right - pr.left - w) / 2,
                 pr.top  + (pr.bottom - pr.top  - h) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    INT_PTR res = runModal(hwnd, parent);
    return (res == IDOK) ? data.result : UINT64_MAX;
}

bool showAlertDialog(HWND parent, const std::wstring& processName,
                     uint32_t pid, AlertPolicy& outPolicy) {
    std::wstring title = processName + L" - 添加报警策略";
    outPolicy.pid         = pid;
    outPolicy.processName = processName;

    AlertData data{processName, pid, &outPolicy};

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, L"NetMonitorAlertDlg", title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, 430, 260, parent, nullptr, GetModuleHandle(nullptr), &data);
    if (!hwnd) return false;

    RECT pr{}, wr{};
    GetWindowRect(parent, &pr);
    GetWindowRect(hwnd, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    SetWindowPos(hwnd, nullptr,
                 pr.left + (pr.right - pr.left - w) / 2,
                 pr.top  + (pr.bottom - pr.top  - h) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    INT_PTR res = runModal(hwnd, parent);
    return res == IDOK;
}

void showAlertListDialog(HWND parent, const std::vector<AlertPolicy>& policies) {
    std::vector<AlertPolicy> copy = policies; // capture snapshot for the dialog

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, LIST_CLASS, L"报警策略列表",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        0, 0, 550, 320, parent, nullptr, GetModuleHandle(nullptr), &copy);
    if (!hwnd) return;

    RECT pr{}, wr{};
    GetWindowRect(parent, &pr);
    GetWindowRect(hwnd, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    SetWindowPos(hwnd, nullptr,
                 pr.left + (pr.right - pr.left - w) / 2,
                 pr.top  + (pr.bottom - pr.top  - h) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    runModal(hwnd, parent);
}

void showAlertNotification(HWND parent, const AlertPolicy& policy) {
    // Build message on heap so the thread owns it
    auto* msg = new std::wstring;
    std::wstringstream ss;
    ss << L"报警触发!\n\n"
       << L"进程: " << policy.processName << L"  (PID: " << policy.pid << L")\n"
       << (policy.direction == Direction::Download ? L"下行" : L"上行")
       << L" 流量在 " << (policy.windowSeconds / 60) << L" 分钟内超过 "
       << (policy.thresholdBytes / 1024 / 1024) << L" MB\n";
    if (policy.limitBytesPerSec > 0) {
        ss << L"\n已自动限速为 " << (policy.limitBytesPerSec / 1024) << L" KB/s";
    }
    *msg = ss.str();

    HWND hwndCapture = parent;
    std::thread([hwndCapture, msg]() {
        MessageBoxW(hwndCapture, msg->c_str(), L"NetMonitor 报警",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        delete msg;
    }).detach();
}

} // namespace Dialogs
