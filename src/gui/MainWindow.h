#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <memory>
#include <map>
#include <vector>

#include "core/Types.h"
#include "core/DivertLimiter.h"
#include "core/TrafficTracker.h"
#include "core/AlertManager.h"
#include "core/EtwMonitor.h"
#include "core/TrafficLogger.h"
#include "core/ConfigStore.h"
#include "resource.h"
#include <chrono>

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    bool create(HINSTANCE hInstance);
    int  run();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void onCreate();
    void onTimer(UINT_PTR timerId);
    void onSize(int cx, int cy);
    void onContextMenu(int x, int y);
    void onCommand(int id);
    void onNotify(NMHDR* nm);
    void onDestroy();

    void restoreConfig();   // Load persisted limits & alert policies on startup
    void saveConfig();      // Persist limits & alert policies on shutdown

    void refreshListView();
    void updateStatusBar();
    void updateToolbarState();
    void applyLimitToProcess(uint32_t pid, Direction dir, uint64_t limitBytesPerSec);

    // Sorting
    void sortSnapshot(std::vector<ProcessTrafficInfo>& vec) const;
    int  compareItems(const ProcessTrafficInfo& a, const ProcessTrafficInfo& b) const;

    // Format helpers
    static std::wstring formatBytes(uint64_t bytes);
    static std::wstring formatRate(double bytesPerSec);
    static std::pair<std::wstring, std::wstring> resolveProcess(uint32_t pid);

    HWND hwnd_      = nullptr;
    HWND listView_  = nullptr;
    HWND statusBar_ = nullptr;
    HWND toolPanel_ = nullptr;   // top button bar
    HINSTANCE hInstance_ = nullptr;

    // Toolbar buttons (stored so we can enable/disable)
    HWND btnDlLimit_  = nullptr;
    HWND btnUlLimit_  = nullptr;
    HWND btnAddAlert_ = nullptr;
    HWND btnRmLimit_  = nullptr;
    HWND btnRmAlert_  = nullptr;
    HWND btnAlerts_   = nullptr;
    HWND btnShowLog_  = nullptr;

    static constexpr int TOOLBAR_H = 38;

    // Core components
    TrafficTracker tracker_;
    AlertManager   alertManager_;
    EtwMonitor     etwMonitor_;
    DivertLimiter  divertLimiter_;
    TrafficLogger  trafficLogger_;
    ConfigStore    configStore_;

    // Session start time (for log flushing on exit)
    std::chrono::system_clock::time_point sessionStart_;

    // Per-process rate limiting state
    struct LimitState {
        uint64_t sendLimit = 0;
        uint64_t recvLimit = 0;
        std::wstring processPath;
    };
    std::map<uint32_t, LimitState> limits_;

    // Limits loaded from config, indexed by processPath.
    // Applied to a PID the first time that process appears in the snapshot.
    // Kept indefinitely so future instances of the same exe are also limited.
    struct PendingLimit { uint64_t sendLimit = 0; uint64_t recvLimit = 0; };
    std::map<std::wstring, PendingLimit> pendingLimitsByPath_;

    // Currently selected PID
    uint32_t selectedPid_ = 0;

    // Sort state
    int  sortCol_  = 2;    // default: sort by download rate
    bool sortAsc_  = false; // descending
};
