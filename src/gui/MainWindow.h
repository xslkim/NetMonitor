#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <memory>
#include <map>
#include <vector>

#include "core/Types.h"
#include "core/TrafficTracker.h"
#include "core/AlertManager.h"
#include "core/EtwMonitor.h"
#include "core/WfpLimiter.h"
#include "core/TokenBucket.h"
#include "resource.h"

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

    void refreshListView();
    void updateStatusBar();
    void updateToolbarState();
    void applyLimits();
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

    static constexpr int TOOLBAR_H = 38;

    // Core components
    TrafficTracker tracker_;
    AlertManager   alertManager_;
    EtwMonitor     etwMonitor_;
    WfpLimiter     wfpLimiter_;

    // Per-process rate limiting state
    struct LimitState {
        std::unique_ptr<TokenBucket> sendBucket;
        std::unique_ptr<TokenBucket> recvBucket;
        uint64_t sendLimit = 0;
        uint64_t recvLimit = 0;
        std::wstring processPath;
    };
    std::map<uint32_t, LimitState> limits_;

    // Currently selected PID
    uint32_t selectedPid_ = 0;

    // Sort state
    int  sortCol_  = 2;    // default: sort by download rate
    bool sortAsc_  = false; // descending
};
