#include "gui/MainWindow.h"
#include "gui/Dialogs.h"
#include <psapi.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "psapi.lib")

static const wchar_t* WINDOW_CLASS = L"NetMonitorMainWindow";

MainWindow::MainWindow() : tracker_(3600) {
    tracker_.setNameResolver([](uint32_t pid) { return resolveProcess(pid); });
}

MainWindow::~MainWindow() = default;

std::pair<std::wstring, std::wstring> MainWindow::resolveProcess(uint32_t pid) {
    std::wstring name, path;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        wchar_t buf[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, buf, &size)) {
            path = buf;
            auto pos = path.find_last_of(L'\\');
            name = (pos != std::wstring::npos) ? path.substr(pos + 1) : path;
        }
        CloseHandle(hProc);
    }
    if (name.empty()) name = L"PID:" + std::to_wstring(pid);
    return {name, path};
}

bool MainWindow::create(HINSTANCE hInstance) {
    hInstance_ = hInstance;

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    Dialogs::registerClasses(hInstance);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = WINDOW_CLASS;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(0, WINDOW_CLASS, L"NetMonitor - 网络流量监控",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1050, 650,
        nullptr, nullptr, hInstance, this);
    if (!hwnd_) return false;

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

int MainWindow::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handleMessage(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:   onCreate(); return 0;
    case WM_SIZE:     onSize(LOWORD(lParam), HIWORD(lParam)); return 0;
    case WM_TIMER:    onTimer(static_cast<UINT_PTR>(wParam)); return 0;
    case WM_COMMAND:  onCommand(LOWORD(wParam)); return 0;
    case WM_DESTROY:  onDestroy(); PostQuitMessage(0); return 0;
    case WM_NOTIFY:
        onNotify(reinterpret_cast<NMHDR*>(lParam));
        return 0;
    case WM_APP + 1: {
        // Alert triggered notification (posted from callback)
        int policyId = static_cast<int>(lParam);
        for (const auto& p : alertManager_.getPolicies()) {
            if (p.id == policyId) {
                Dialogs::showAlertNotification(hwnd_, p);
                break;
            }
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void MainWindow::onCreate() {
    // ── Toolbar panel ─────────────────────────────────────────────
    toolPanel_ = CreateWindowW(L"STATIC", nullptr,
        WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
        0, 0, 0, TOOLBAR_H, hwnd_, (HMENU)IDC_TOOLBAR_PANEL, hInstance_, nullptr);

    auto makeBtn = [&](const wchar_t* text, int id, int x) -> HWND {
        return CreateWindowW(L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            x, 5, 110, 28, hwnd_, (HMENU)(INT_PTR)id, hInstance_, nullptr);
    };

    int x = 8;
    btnDlLimit_  = makeBtn(L"↓ 设置下行限速", IDM_SET_DL_LIMIT,  x); x += 118;
    btnUlLimit_  = makeBtn(L"↑ 设置上行限速", IDM_SET_UL_LIMIT,  x); x += 118;
    btnAddAlert_ = makeBtn(L"+ 添加报警策略", IDM_ADD_ALERT,      x); x += 118;
    btnRmLimit_  = makeBtn(L"× 清除限速",     IDM_REMOVE_LIMITS,  x); x += 118;
    btnRmAlert_  = makeBtn(L"× 清除报警",     IDM_REMOVE_ALERTS,  x); x += 128;
    btnAlerts_   = makeBtn(L"≡ 报警策略列表", IDM_SHOW_ALERTS,    x);

    // Add a hint label after the buttons
    CreateWindowW(L"STATIC",
        L"提示: 先点击列表中的进程行，再使用上方按钮操作；也可右键点击进程行",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x + 120, 12, 350, 20, hwnd_, nullptr, hInstance_, nullptr);

    updateToolbarState();

    // ── ListView ──────────────────────────────────────────────────
    listView_ = CreateWindowExW(0, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, TOOLBAR_H, 0, 0, hwnd_, (HMENU)IDC_LISTVIEW, hInstance_, nullptr);

    ListView_SetExtendedListViewStyle(listView_,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    struct ColInfo { const wchar_t* name; int width; };
    ColInfo cols[] = {
        {L"PID",        60},
        {L"进程名",    140},
        {L"下行速率",  110},
        {L"上行速率",  110},
        {L"总下行",    100},
        {L"总上行",    100},
        {L"下行限速",   90},
        {L"上行限速",   90},
        {L"状态",       90},
    };
    for (int i = 0; i < _countof(cols); i++) {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<LPWSTR>(cols[i].name);
        col.cx = cols[i].width;
        col.iSubItem = i;
        ListView_InsertColumn(listView_, i, &col);
    }

    // ── Status bar ────────────────────────────────────────────────
    statusBar_ = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd_, (HMENU)IDC_STATUSBAR, hInstance_, nullptr);

    // ── ETW monitor ───────────────────────────────────────────────
    etwMonitor_.setCallback([this](uint32_t pid, uint32_t bytes, Direction dir) {
        tracker_.addTraffic(pid, bytes, dir);
    });

    if (!etwMonitor_.start()) {
        MessageBoxW(hwnd_,
            (L"ETW 启动失败: " + etwMonitor_.getLastError()).c_str(),
            L"警告", MB_OK | MB_ICONWARNING);
    }

    // ── WinDivert limiter ─────────────────────────────────────────
    if (!divertLimiter_.start()) {
        MessageBoxW(hwnd_,
            (L"WinDivert 启动失败: " + divertLimiter_.getLastError()).c_str(),
            L"警告", MB_OK | MB_ICONWARNING);
    }

    // ── Alert callback ────────────────────────────────────────────
    alertManager_.setAlertCallback([this](const AlertPolicy& policy, const AlertAction& action) {
        applyLimitToProcess(action.pid, action.direction, action.limitBytesPerSec);
        PostMessageW(hwnd_, WM_APP + 1, 0, static_cast<LPARAM>(policy.id));
    });

    // ── Timers ────────────────────────────────────────────────────
    SetTimer(hwnd_, IDT_REFRESH, 1000, nullptr);
}

void MainWindow::applyLimitToProcess(uint32_t pid, Direction dir, uint64_t limitBytesPerSec) {
    auto& ls = limits_[pid];

    if (limitBytesPerSec > 0) {
        divertLimiter_.setLimit(pid, dir, limitBytesPerSec);
    } else {
        divertLimiter_.removeLimit(pid, dir);
    }

    if (dir == Direction::Download) {
        ls.recvLimit = limitBytesPerSec;
    } else {
        ls.sendLimit = limitBytesPerSec;
    }

    if (ls.sendLimit == 0 && ls.recvLimit == 0) {
        limits_.erase(pid);
    }
}

void MainWindow::onTimer(UINT_PTR timerId) {
    if (timerId == IDT_REFRESH) {
        tracker_.update();
        alertManager_.evaluate([this](uint32_t pid, int window, Direction dir) -> uint64_t {
            return tracker_.getTrafficInWindow(pid, window, dir);
        });
        refreshListView();
        updateStatusBar();
        tracker_.pruneInactive(30);
    }
}

void MainWindow::onSize(int cx, int cy) {
    if (statusBar_) {
        SendMessageW(statusBar_, WM_SIZE, 0, 0);
        RECT sbRect;
        GetWindowRect(statusBar_, &sbRect);
        int sbH = sbRect.bottom - sbRect.top;

        // Toolbar panel buttons stay at top
        if (toolPanel_) MoveWindow(toolPanel_, 0, 0, cx, TOOLBAR_H, TRUE);
        // Reposition all toolbar buttons
        auto moveChild = [&](HWND h, int x) {
            if (h) { RECT r; GetWindowRect(h, &r); MapWindowPoints(nullptr, hwnd_, (POINT*)&r, 2);
                     MoveWindow(h, x, 5, 110, 28, TRUE); }
        };
        // Re-layout handled by initial positions (buttons have fixed size)

        if (listView_)
            MoveWindow(listView_, 0, TOOLBAR_H, cx, cy - TOOLBAR_H - sbH, TRUE);
    }
}

void MainWindow::onNotify(NMHDR* nm) {
    if (nm->idFrom == IDC_LISTVIEW) {
        if (nm->code == NM_RCLICK) {
            POINT pt;
            GetCursorPos(&pt);
            onContextMenu(pt.x, pt.y);
        } else if (nm->code == LVN_COLUMNCLICK) {
            auto* nml = reinterpret_cast<NMLISTVIEW*>(nm);
            int col = nml->iSubItem;
            if (col == sortCol_) sortAsc_ = !sortAsc_;
            else { sortCol_ = col; sortAsc_ = (col == 1); } // text cols default ascending
            refreshListView();
        } else if (nm->code == LVN_ITEMCHANGED) {
            auto* nml = reinterpret_cast<NMLISTVIEW*>(nm);
            if ((nml->uChanged & LVIF_STATE) && (nml->uNewState & LVIS_SELECTED)) {
                wchar_t buf[32] = {};
                ListView_GetItemText(listView_, nml->iItem, 0, buf, 32);
                selectedPid_ = static_cast<uint32_t>(_wtoi(buf));
                updateToolbarState();
            }
        }
    }
}

void MainWindow::onContextMenu(int x, int y) {
    int sel = ListView_GetNextItem(listView_, -1, LVNI_SELECTED);
    if (sel < 0) return;

    wchar_t pidBuf[32] = {};
    ListView_GetItemText(listView_, sel, 0, pidBuf, 32);
    selectedPid_ = static_cast<uint32_t>(_wtoi(pidBuf));

    auto it = limits_.find(selectedPid_);
    uint64_t dlLim = it != limits_.end() ? it->second.recvLimit : 0;
    uint64_t ulLim = it != limits_.end() ? it->second.sendLimit : 0;

    HMENU hMenu = CreatePopupMenu();
    std::wstring dlText = L"↓ 设置下行限速";
    if (dlLim > 0) dlText += L"  (当前: " + std::to_wstring(dlLim / 1024) + L" KB/s)";
    std::wstring ulText = L"↑ 设置上行限速";
    if (ulLim > 0) ulText += L"  (当前: " + std::to_wstring(ulLim / 1024) + L" KB/s)";

    AppendMenuW(hMenu, MF_STRING, IDM_SET_DL_LIMIT, dlText.c_str());
    AppendMenuW(hMenu, MF_STRING, IDM_SET_UL_LIMIT, ulText.c_str());
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_ADD_ALERT,     L"+ 添加报警策略...");
    AppendMenuW(hMenu, MF_STRING, IDM_SHOW_ALERTS,   L"≡ 查看所有报警策略...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING | (dlLim == 0 && ulLim == 0 ? MF_GRAYED : 0),
                IDM_REMOVE_LIMITS, L"× 清除限速");
    AppendMenuW(hMenu, MF_STRING, IDM_REMOVE_ALERTS, L"× 清除该进程的报警");

    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, x, y, 0, hwnd_, nullptr);
    DestroyMenu(hMenu);
}

void MainWindow::onCommand(int id) {
    if (id == IDM_SHOW_ALERTS) {
        Dialogs::showAlertListDialog(hwnd_, alertManager_.getPolicies());
        return;
    }
    if (selectedPid_ == 0 && id != IDM_SHOW_ALERTS) return;

    auto snapshot = tracker_.getSnapshot();
    auto it = snapshot.find(selectedPid_);
    std::wstring procName = (it != snapshot.end()) ? it->second.processName : L"Unknown";
    auto limIt = limits_.find(selectedPid_);

    switch (id) {
    case IDM_SET_DL_LIMIT: {
        uint64_t cur = (limIt != limits_.end()) ? limIt->second.recvLimit : 0;
        uint64_t limit = Dialogs::showLimitDialog(hwnd_, procName, Direction::Download, cur);
        if (limit != UINT64_MAX)
            applyLimitToProcess(selectedPid_, Direction::Download, limit);
        break;
    }
    case IDM_SET_UL_LIMIT: {
        uint64_t cur = (limIt != limits_.end()) ? limIt->second.sendLimit : 0;
        uint64_t limit = Dialogs::showLimitDialog(hwnd_, procName, Direction::Upload, cur);
        if (limit != UINT64_MAX)
            applyLimitToProcess(selectedPid_, Direction::Upload, limit);
        break;
    }
    case IDM_ADD_ALERT: {
        AlertPolicy policy;
        if (Dialogs::showAlertDialog(hwnd_, procName, selectedPid_, policy))
            alertManager_.addPolicy(std::move(policy));
        break;
    }
    case IDM_REMOVE_LIMITS:
        applyLimitToProcess(selectedPid_, Direction::Download, 0);
        applyLimitToProcess(selectedPid_, Direction::Upload,   0);
        break;
    case IDM_REMOVE_ALERTS: {
        auto policies = alertManager_.getPolicies();
        for (const auto& p : policies)
            if (p.pid == selectedPid_) alertManager_.removePolicy(p.id);
        break;
    }
    }
    updateToolbarState();
}

// ── Sort helpers ──────────────────────────────────────────────────────────────

void MainWindow::sortSnapshot(std::vector<ProcessTrafficInfo>& vec) const {
    std::stable_sort(vec.begin(), vec.end(),
        [this](const ProcessTrafficInfo& a, const ProcessTrafficInfo& b) {
            int cmp = compareItems(a, b);
            return sortAsc_ ? cmp < 0 : cmp > 0;
        });
}

int MainWindow::compareItems(const ProcessTrafficInfo& a, const ProcessTrafficInfo& b) const {
    switch (sortCol_) {
    case 0: return (int)a.pid - (int)b.pid;
    case 1: return a.processName.compare(b.processName);
    case 2: return (a.recvRate > b.recvRate) ? 1 : (a.recvRate < b.recvRate) ? -1 : 0;
    case 3: return (a.sendRate > b.sendRate) ? 1 : (a.sendRate < b.sendRate) ? -1 : 0;
    case 4: return (a.totalRecv > b.totalRecv) ? 1 : (a.totalRecv < b.totalRecv) ? -1 : 0;
    case 5: return (a.totalSent > b.totalSent) ? 1 : (a.totalSent < b.totalSent) ? -1 : 0;
    case 6: return (int64_t)a.recvLimit - (int64_t)b.recvLimit;
    case 7: return (int64_t)a.sendLimit - (int64_t)b.sendLimit;
    default: return 0;
    }
}

// ── List view refresh ─────────────────────────────────────────────────────────

void MainWindow::refreshListView() {
    auto snapMap = tracker_.getSnapshot();

    // Build a vector for sorting
    std::vector<ProcessTrafficInfo> items;
    items.reserve(snapMap.size());
    for (auto& [pid, info] : snapMap) {
        if (info.sendRate < 1.0 && info.recvRate < 1.0 &&
            info.totalSent == 0 && info.totalRecv == 0) continue;
        // Merge limit state
        auto limIt = limits_.find(pid);
        if (limIt != limits_.end()) {
            info.sendLimit   = limIt->second.sendLimit;
            info.recvLimit   = limIt->second.recvLimit;
            info.sendBlocked = limIt->second.sendLimit > 0;
            info.recvBlocked = limIt->second.recvLimit > 0;
        }
        items.push_back(std::move(info));
    }
    sortSnapshot(items);

    SendMessageW(listView_, WM_SETREDRAW, FALSE, 0);

    int targetCount = (int)items.size();
    int curCount    = ListView_GetItemCount(listView_);

    // Remove excess rows
    while (ListView_GetItemCount(listView_) > targetCount)
        ListView_DeleteItem(listView_, ListView_GetItemCount(listView_) - 1);

    // Add missing rows
    while (ListView_GetItemCount(listView_) < targetCount) {
        LVITEMW lvi = {};
        lvi.mask  = LVIF_TEXT;
        lvi.iItem = ListView_GetItemCount(listView_);
        lvi.pszText = const_cast<LPWSTR>(L"");
        ListView_InsertItem(listView_, &lvi);
    }

    // Update all rows
    for (int row = 0; row < targetCount; row++) {
        const auto& info = items[row];
        auto setText = [&](int col, const std::wstring& text) {
            ListView_SetItemText(listView_, row, col,
                                 const_cast<LPWSTR>(text.c_str()));
        };
        setText(0, std::to_wstring(info.pid));
        setText(1, info.processName);
        setText(2, formatRate(info.recvRate));
        setText(3, formatRate(info.sendRate));
        setText(4, formatBytes(info.totalRecv));
        setText(5, formatBytes(info.totalSent));
        setText(6, info.recvLimit > 0 ? formatRate((double)info.recvLimit) : L"无限制");
        setText(7, info.sendLimit > 0 ? formatRate((double)info.sendLimit) : L"无限制");
        std::wstring st;
        if (info.recvBlocked) st += L"↓限速 ";
        if (info.sendBlocked) st += L"↑限速 ";
        if (st.empty()) st = L"正常";
        setText(8, st);
    }

    // Draw sort indicator on column header
    HWND hHeader = ListView_GetHeader(listView_);
    int colCount = Header_GetItemCount(hHeader);
    for (int i = 0; i < colCount; i++) {
        HDITEMW hi = {};
        hi.mask = HDI_FORMAT;
        Header_GetItem(hHeader, i, &hi);
        hi.fmt &= ~(HDF_SORTDOWN | HDF_SORTUP);
        if (i == sortCol_) hi.fmt |= sortAsc_ ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(hHeader, i, &hi);
    }

    SendMessageW(listView_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(listView_, nullptr, FALSE);
}

void MainWindow::updateStatusBar() {
    auto snapshot = tracker_.getSnapshot();
    int cnt = 0; double dl = 0, ul = 0;
    for (const auto& [pid, info] : snapshot) {
        if (info.sendRate > 0 || info.recvRate > 0) { cnt++; dl += info.recvRate; ul += info.sendRate; }
    }
    int alertsTotal = 0, alertsTriggered = 0;
    for (const auto& p : alertManager_.getPolicies()) {
        alertsTotal++;
        if (p.triggered) alertsTriggered++;
    }
    std::wstringstream ss;
    ss << L"活跃进程: " << cnt
       << L"   总下行: " << formatRate(dl)
       << L"   总上行: " << formatRate(ul)
       << L"   报警: " << alertsTriggered << L"/" << alertsTotal << L" 已触发"
       << L"   ETW: " << (etwMonitor_.isRunning() ? L"运行中" : L"未启动")
       << L"   Divert: " << (divertLimiter_.isRunning() ? L"运行中" : L"未启动");
    SetWindowTextW(statusBar_, ss.str().c_str());
}

void MainWindow::updateToolbarState() {
    bool hasSel = (selectedPid_ != 0);
    auto limIt = limits_.find(selectedPid_);
    bool hasLimit = hasSel && limIt != limits_.end() &&
                    (limIt->second.sendLimit > 0 || limIt->second.recvLimit > 0);

    EnableWindow(btnDlLimit_,  hasSel);
    EnableWindow(btnUlLimit_,  hasSel);
    EnableWindow(btnAddAlert_, hasSel);
    EnableWindow(btnRmLimit_,  hasLimit);
    EnableWindow(btnRmAlert_,  hasSel);
}

void MainWindow::onDestroy() {
    KillTimer(hwnd_, IDT_REFRESH);
    divertLimiter_.clearAllLimits();
    divertLimiter_.stop();
    etwMonitor_.stop();
}

std::wstring MainWindow::formatBytes(uint64_t bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + L" B";
    if (bytes < 1024 * 1024) {
        std::wstringstream ss; ss << std::fixed << std::setprecision(1) << bytes / 1024.0 << L" KB"; return ss.str();
    }
    if (bytes < 1024ULL * 1024 * 1024) {
        std::wstringstream ss; ss << std::fixed << std::setprecision(1) << bytes / 1024.0 / 1024.0 << L" MB"; return ss.str();
    }
    std::wstringstream ss; ss << std::fixed << std::setprecision(2) << bytes / 1024.0 / 1024.0 / 1024.0 << L" GB"; return ss.str();
}

std::wstring MainWindow::formatRate(double bytesPerSec) {
    if (bytesPerSec < 1.0) return L"0 B/s";
    return formatBytes(static_cast<uint64_t>(bytesPerSec)) + L"/s";
}
