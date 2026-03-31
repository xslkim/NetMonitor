#include "gui/MainWindow.h"
#include "gui/Dialogs.h"
#include <psapi.h>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "psapi.lib")

static const wchar_t* WINDOW_CLASS = L"NetMonitorMainWindow";

MainWindow::MainWindow() : tracker_(3600) {
    tracker_.setNameResolver([](uint32_t pid) {
        return resolveProcess(pid);
    });
}

MainWindow::~MainWindow() = default;

std::pair<std::wstring, std::wstring> MainWindow::resolveProcess(uint32_t pid) {
    std::wstring name = L"<unknown>";
    std::wstring path;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        wchar_t buf[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, buf, &size)) {
            path = buf;
            // Extract filename
            auto pos = path.find_last_of(L'\\');
            name = (pos != std::wstring::npos) ? path.substr(pos + 1) : path;
        }
        CloseHandle(hProc);
    }

    if (name == L"<unknown>") {
        name = L"PID:" + std::to_wstring(pid);
    }

    return {name, path};
}

bool MainWindow::create(HINSTANCE hInstance) {
    hInstance_ = hInstance;

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = WINDOW_CLASS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(0, WINDOW_CLASS, L"NetMonitor - 网络流量监控",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 950, 600,
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

    if (self) {
        return self->handleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        onCreate();
        return 0;
    case WM_SIZE:
        onSize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_TIMER:
        onTimer(static_cast<UINT_PTR>(wParam));
        return 0;
    case WM_NOTIFY: {
        auto* nmhdr = reinterpret_cast<NMHDR*>(lParam);
        if (nmhdr->idFrom == IDC_LISTVIEW && nmhdr->code == NM_RCLICK) {
            POINT pt;
            GetCursorPos(&pt);
            onContextMenu(pt.x, pt.y);
        }
        return 0;
    }
    case WM_COMMAND:
        onCommand(LOWORD(wParam));
        return 0;
    case WM_DESTROY:
        onDestroy();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void MainWindow::onCreate() {
    // Create ListView
    listView_ = CreateWindowExW(0, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, hwnd_, (HMENU)IDC_LISTVIEW, hInstance_, nullptr);

    ListView_SetExtendedListViewStyle(listView_,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    // Add columns
    struct ColInfo { const wchar_t* name; int width; };
    ColInfo cols[] = {
        {L"PID",        60},
        {L"进程名",     140},
        {L"下行速率",    100},
        {L"上行速率",    100},
        {L"总下行",      100},
        {L"总上行",      100},
        {L"下行限速",    90},
        {L"上行限速",    90},
        {L"状态",       100},
    };

    for (int i = 0; i < _countof(cols); i++) {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<LPWSTR>(cols[i].name);
        col.cx = cols[i].width;
        col.iSubItem = i;
        ListView_InsertColumn(listView_, i, &col);
    }

    // Create status bar
    statusBar_ = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd_, (HMENU)IDC_STATUSBAR, hInstance_, nullptr);

    // Start ETW monitor
    etwMonitor_.setCallback([this](uint32_t pid, uint32_t bytes, Direction dir) {
        tracker_.addTraffic(pid, bytes, dir);
    });

    if (!etwMonitor_.start()) {
        std::wstring err = L"ETW 启动失败: " + etwMonitor_.getLastError();
        MessageBoxW(hwnd_, err.c_str(), L"警告", MB_OK | MB_ICONWARNING);
    }

    // Initialize WFP limiter
    if (!wfpLimiter_.init()) {
        std::wstring err = L"WFP 初始化失败: " + wfpLimiter_.getLastError();
        MessageBoxW(hwnd_, err.c_str(), L"警告", MB_OK | MB_ICONWARNING);
    }

    // Set alert callback
    alertManager_.setAlertCallback([this](const AlertPolicy& policy, const AlertAction& action) {
        // Apply rate limit
        auto& ls = limits_[action.pid];
        if (action.direction == Direction::Download) {
            ls.recvLimit = action.limitBytesPerSec;
            ls.recvBucket = std::make_unique<TokenBucket>(action.limitBytesPerSec, action.limitBytesPerSec);
        } else {
            ls.sendLimit = action.limitBytesPerSec;
            ls.sendBucket = std::make_unique<TokenBucket>(action.limitBytesPerSec, action.limitBytesPerSec);
        }
        if (ls.processPath.empty()) {
            auto [name, path] = resolveProcess(action.pid);
            ls.processPath = path;
        }

        // Show notification asynchronously
        PostMessageW(hwnd_, WM_APP + 1, 0, static_cast<LPARAM>(policy.id));
    });

    // Start timers
    SetTimer(hwnd_, IDT_REFRESH, 1000, nullptr);   // 1 second UI refresh
    SetTimer(hwnd_, IDT_LIMITER, 100, nullptr);     // 100ms limiter check
}

void MainWindow::onTimer(UINT_PTR timerId) {
    if (timerId == IDT_REFRESH) {
        tracker_.update();

        // Evaluate alert policies
        alertManager_.evaluate([this](uint32_t pid, int window, Direction dir) -> uint64_t {
            return tracker_.getTrafficInWindow(pid, window, dir);
        });

        refreshListView();
        updateStatusBar();
        tracker_.pruneInactive(30);
    } else if (timerId == IDT_LIMITER) {
        applyLimits();
    }
}

void MainWindow::applyLimits() {
    for (auto& [pid, ls] : limits_) {
        if (ls.processPath.empty()) continue;

        // Download limit
        if (ls.recvBucket && ls.recvLimit > 0) {
            // Replenish happens automatically via time-based token bucket
            // Check if there are tokens available
            if (ls.recvBucket->available() > 0) {
                wfpLimiter_.unblockProcess(pid, Direction::Download);
            }
        }

        // Upload limit
        if (ls.sendBucket && ls.sendLimit > 0) {
            if (ls.sendBucket->available() > 0) {
                wfpLimiter_.unblockProcess(pid, Direction::Upload);
            }
        }
    }
}

void MainWindow::onSize(int cx, int cy) {
    if (statusBar_) {
        SendMessageW(statusBar_, WM_SIZE, 0, 0);
        RECT sbRect;
        GetWindowRect(statusBar_, &sbRect);
        int sbHeight = sbRect.bottom - sbRect.top;
        if (listView_) {
            MoveWindow(listView_, 0, 0, cx, cy - sbHeight, TRUE);
        }
    }
}

void MainWindow::onContextMenu(int x, int y) {
    // Get selected item
    int sel = ListView_GetNextItem(listView_, -1, LVNI_SELECTED);
    if (sel < 0) return;

    wchar_t pidBuf[32] = {};
    ListView_GetItemText(listView_, sel, 0, pidBuf, 32);
    selectedPid_ = static_cast<uint32_t>(_wtoi(pidBuf));

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_SET_DL_LIMIT, L"设置下行限速...");
    AppendMenuW(hMenu, MF_STRING, IDM_SET_UL_LIMIT, L"设置上行限速...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_ADD_ALERT, L"添加报警策略...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_REMOVE_LIMITS, L"移除限速");
    AppendMenuW(hMenu, MF_STRING, IDM_REMOVE_ALERTS, L"移除报警");

    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, x, y, 0, hwnd_, nullptr);
    DestroyMenu(hMenu);
}

void MainWindow::onCommand(int id) {
    if (selectedPid_ == 0) return;

    auto snapshot = tracker_.getSnapshot();
    auto it = snapshot.find(selectedPid_);
    std::wstring procName = (it != snapshot.end()) ? it->second.processName : L"Unknown";

    switch (id) {
    case IDM_SET_DL_LIMIT: {
        uint64_t limit = Dialogs::showLimitDialog(hwnd_, procName, Direction::Download);
        auto& ls = limits_[selectedPid_];
        if (ls.processPath.empty()) {
            auto [name, path] = resolveProcess(selectedPid_);
            ls.processPath = path;
        }
        if (limit > 0) {
            ls.recvLimit = limit;
            ls.recvBucket = std::make_unique<TokenBucket>(limit, limit);
        } else {
            ls.recvLimit = 0;
            ls.recvBucket.reset();
            wfpLimiter_.unblockProcess(selectedPid_, Direction::Download);
        }
        break;
    }
    case IDM_SET_UL_LIMIT: {
        uint64_t limit = Dialogs::showLimitDialog(hwnd_, procName, Direction::Upload);
        auto& ls = limits_[selectedPid_];
        if (ls.processPath.empty()) {
            auto [name, path] = resolveProcess(selectedPid_);
            ls.processPath = path;
        }
        if (limit > 0) {
            ls.sendLimit = limit;
            ls.sendBucket = std::make_unique<TokenBucket>(limit, limit);
        } else {
            ls.sendLimit = 0;
            ls.sendBucket.reset();
            wfpLimiter_.unblockProcess(selectedPid_, Direction::Upload);
        }
        break;
    }
    case IDM_ADD_ALERT: {
        AlertPolicy policy;
        if (Dialogs::showAlertDialog(hwnd_, procName, selectedPid_, policy)) {
            alertManager_.addPolicy(std::move(policy));
        }
        break;
    }
    case IDM_REMOVE_LIMITS: {
        auto& ls = limits_[selectedPid_];
        ls.sendLimit = 0;
        ls.recvLimit = 0;
        ls.sendBucket.reset();
        ls.recvBucket.reset();
        wfpLimiter_.unblockProcess(selectedPid_, Direction::Upload);
        wfpLimiter_.unblockProcess(selectedPid_, Direction::Download);
        break;
    }
    case IDM_REMOVE_ALERTS: {
        auto policies = alertManager_.getPolicies();
        for (const auto& p : policies) {
            if (p.pid == selectedPid_) {
                alertManager_.removePolicy(p.id);
            }
        }
        break;
    }
    }
}

void MainWindow::refreshListView() {
    auto snapshot = tracker_.getSnapshot();

    // Merge limit info into snapshot
    for (auto& [pid, info] : snapshot) {
        auto limIt = limits_.find(pid);
        if (limIt != limits_.end()) {
            info.sendLimit = limIt->second.sendLimit;
            info.recvLimit = limIt->second.recvLimit;
            info.sendBlocked = wfpLimiter_.isBlocked(pid, Direction::Upload);
            info.recvBlocked = wfpLimiter_.isBlocked(pid, Direction::Download);
        }
    }

    // Update ListView - preserve selection
    SendMessageW(listView_, WM_SETREDRAW, FALSE, 0);

    // Build map of existing items for efficient update
    int itemCount = ListView_GetItemCount(listView_);
    std::map<uint32_t, int> existingItems;
    for (int i = 0; i < itemCount; i++) {
        wchar_t buf[32];
        ListView_GetItemText(listView_, i, 0, buf, 32);
        existingItems[static_cast<uint32_t>(_wtoi(buf))] = i;
    }

    // Remove items for processes no longer in snapshot
    for (auto it = existingItems.rbegin(); it != existingItems.rend(); ++it) {
        if (snapshot.find(it->first) == snapshot.end()) {
            ListView_DeleteItem(listView_, it->second);
        }
    }

    // Add/update items
    int idx = 0;
    for (const auto& [pid, info] : snapshot) {
        // Skip processes with zero traffic
        if (info.sendRate < 1.0 && info.recvRate < 1.0 &&
            info.totalSent == 0 && info.totalRecv == 0) continue;

        auto existing = existingItems.find(pid);
        bool isNew = (existing == existingItems.end());

        if (isNew) {
            LVITEMW lvi = {};
            lvi.mask = LVIF_TEXT;
            lvi.iItem = idx;
            std::wstring pidStr = std::to_wstring(pid);
            lvi.pszText = const_cast<LPWSTR>(pidStr.c_str());
            ListView_InsertItem(listView_, &lvi);
        }

        int row = isNew ? idx : existing->second;

        auto setText = [&](int col, const std::wstring& text) {
            ListView_SetItemText(listView_, row, col, const_cast<LPWSTR>(text.c_str()));
        };

        setText(0, std::to_wstring(pid));
        setText(1, info.processName);
        setText(2, formatRate(info.recvRate));
        setText(3, formatRate(info.sendRate));
        setText(4, formatBytes(info.totalRecv));
        setText(5, formatBytes(info.totalSent));
        setText(6, info.recvLimit > 0 ? formatRate(static_cast<double>(info.recvLimit)) : L"无限制");
        setText(7, info.sendLimit > 0 ? formatRate(static_cast<double>(info.sendLimit)) : L"无限制");

        std::wstring status;
        if (info.recvBlocked) status += L"↓阻断 ";
        if (info.sendBlocked) status += L"↑阻断 ";
        if (status.empty()) status = L"正常";
        setText(8, status);

        idx++;
    }

    SendMessageW(listView_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(listView_, nullptr, FALSE);
}

void MainWindow::updateStatusBar() {
    auto snapshot = tracker_.getSnapshot();
    int processCount = 0;
    double totalDl = 0, totalUl = 0;
    for (const auto& [pid, info] : snapshot) {
        if (info.sendRate > 0 || info.recvRate > 0) {
            processCount++;
            totalDl += info.recvRate;
            totalUl += info.sendRate;
        }
    }

    int alertCount = 0;
    for (const auto& p : alertManager_.getPolicies()) {
        if (p.triggered) alertCount++;
    }

    std::wstringstream ss;
    ss << L"活跃进程: " << processCount
       << L"  |  总下行: " << formatRate(totalDl)
       << L"  |  总上行: " << formatRate(totalUl)
       << L"  |  已触发报警: " << alertCount
       << L"  |  ETW: " << (etwMonitor_.isRunning() ? L"运行中" : L"未启动")
       << L"  |  WFP: " << (wfpLimiter_.isInitialized() ? L"就绪" : L"未初始化");

    SetWindowTextW(statusBar_, ss.str().c_str());
}

void MainWindow::onDestroy() {
    KillTimer(hwnd_, IDT_REFRESH);
    KillTimer(hwnd_, IDT_LIMITER);

    // Remove all WFP blocks
    for (auto& [pid, ls] : limits_) {
        wfpLimiter_.unblockProcess(pid, Direction::Upload);
        wfpLimiter_.unblockProcess(pid, Direction::Download);
    }

    etwMonitor_.stop();
    wfpLimiter_.cleanup();
}

std::wstring MainWindow::formatBytes(uint64_t bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + L" B";
    if (bytes < 1024 * 1024) {
        std::wstringstream ss;
        ss << std::fixed << std::setprecision(1) << (bytes / 1024.0) << L" KB";
        return ss.str();
    }
    if (bytes < 1024ULL * 1024 * 1024) {
        std::wstringstream ss;
        ss << std::fixed << std::setprecision(1) << (bytes / 1024.0 / 1024.0) << L" MB";
        return ss.str();
    }
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(2) << (bytes / 1024.0 / 1024.0 / 1024.0) << L" GB";
    return ss.str();
}

std::wstring MainWindow::formatRate(double bytesPerSec) {
    if (bytesPerSec < 1.0) return L"0 B/s";
    return formatBytes(static_cast<uint64_t>(bytesPerSec)) + L"/s";
}
