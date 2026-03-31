#pragma once

#include "core/Types.h"
#include <map>
#include <deque>
#include <mutex>
#include <functional>

struct PerSecondSnapshot {
    std::chrono::steady_clock::time_point timestamp;
    uint64_t totalSent = 0;
    uint64_t totalRecv = 0;
};

struct ProcessTrafficData {
    uint32_t pid = 0;
    std::wstring processName;
    std::wstring processPath;
    uint64_t totalSent = 0;
    uint64_t totalRecv = 0;
    double sendRate = 0.0;
    double recvRate = 0.0;
    std::deque<PerSecondSnapshot> history;
    std::chrono::steady_clock::time_point lastActiveTime{};
};

class TrafficTracker {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using ResolveNameFn = std::function<std::pair<std::wstring, std::wstring>(uint32_t pid)>;

    explicit TrafficTracker(int maxHistorySeconds = 3600);

    void setNameResolver(ResolveNameFn fn) { nameResolver_ = std::move(fn); }

    // Called from ETW callback (thread-safe)
    void addTraffic(uint32_t pid, uint32_t bytes, Direction dir);

    // Called periodically to compute rates and take snapshots
    void update();

    // Get traffic in a time window for a specific process
    uint64_t getTrafficInWindow(uint32_t pid, int windowSeconds, Direction dir) const;

    // Get snapshot of all processes
    std::map<uint32_t, ProcessTrafficInfo> getSnapshot() const;

    // Remove processes with no traffic for a while
    void pruneInactive(int inactiveSeconds = 30);

    // Test support
    void setTimeForTest(TimePoint tp);
    void advanceTimeForTest(std::chrono::milliseconds ms);

private:
    void ensureProcess(uint32_t pid);

    int maxHistorySeconds_;
    std::map<uint32_t, ProcessTrafficData> processes_;
    mutable std::mutex mutex_;
    ResolveNameFn nameResolver_;

    bool useTestTime_ = false;
    TimePoint testTime_{};
    TimePoint now() const;
};
