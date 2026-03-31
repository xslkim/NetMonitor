#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fwpmu.h>
#include <map>
#include <string>
#include <mutex>

#include "core/Types.h"

class WfpLimiter {
public:
    WfpLimiter();
    ~WfpLimiter();

    bool init();
    void cleanup();
    bool isInitialized() const { return engine_ != nullptr; }

    // Block all new and ongoing traffic for a process in a direction
    bool blockProcess(uint32_t pid, const std::wstring& processPath, Direction dir);

    // Unblock a previously blocked process
    bool unblockProcess(uint32_t pid, Direction dir);

    // Check if a process is currently blocked
    bool isBlocked(uint32_t pid, Direction dir) const;

    std::wstring getLastError() const { return lastError_; }

private:
    struct FilterEntry {
        UINT64 filterId = 0;
        bool active = false;
    };

    // Key: (pid << 1) | direction
    uint64_t makeKey(uint32_t pid, Direction dir) const {
        return (static_cast<uint64_t>(pid) << 1) | static_cast<uint64_t>(dir);
    }

    HANDLE engine_ = nullptr;
    GUID sublayerGuid_{};
    std::map<uint64_t, FilterEntry> filters_;
    mutable std::mutex mutex_;
    std::wstring lastError_;
};
