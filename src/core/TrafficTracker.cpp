#include "core/TrafficTracker.h"
#include <algorithm>

TrafficTracker::TrafficTracker(int maxHistorySeconds)
    : maxHistorySeconds_(maxHistorySeconds) {}

TrafficTracker::TimePoint TrafficTracker::now() const {
    return useTestTime_ ? testTime_ : Clock::now();
}

void TrafficTracker::setTimeForTest(TimePoint tp) {
    std::lock_guard<std::mutex> lock(mutex_);
    useTestTime_ = true;
    testTime_ = tp;
}

void TrafficTracker::advanceTimeForTest(std::chrono::milliseconds ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    testTime_ += ms;
}

void TrafficTracker::ensureProcess(uint32_t pid) {
    if (processes_.find(pid) == processes_.end()) {
        auto& p = processes_[pid];
        p.pid = pid;
        if (nameResolver_) {
            auto [name, path] = nameResolver_(pid);
            p.processName = std::move(name);
            p.processPath = std::move(path);
        } else {
            p.processName = L"PID:" + std::to_wstring(pid);
        }
    }
}

void TrafficTracker::addTraffic(uint32_t pid, uint32_t bytes, Direction dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensureProcess(pid);
    auto& proc = processes_[pid];
    if (dir == Direction::Upload) {
        proc.totalSent += bytes;
    } else {
        proc.totalRecv += bytes;
    }
    proc.lastActiveTime = now();
}

void TrafficTracker::update() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto currentTime = now();

    for (auto& [pid, proc] : processes_) {
        // Calculate rate from last snapshot
        if (!proc.history.empty()) {
            auto& last = proc.history.back();
            double elapsed = std::chrono::duration<double>(currentTime - last.timestamp).count();
            if (elapsed > 0.0) {
                proc.sendRate = static_cast<double>(proc.totalSent - last.totalSent) / elapsed;
                proc.recvRate = static_cast<double>(proc.totalRecv - last.totalRecv) / elapsed;
            }
        }

        // Take snapshot
        PerSecondSnapshot snap;
        snap.timestamp = currentTime;
        snap.totalSent = proc.totalSent;
        snap.totalRecv = proc.totalRecv;
        proc.history.push_back(snap);

        // Trim old history
        auto cutoff = currentTime - std::chrono::seconds(maxHistorySeconds_);
        while (!proc.history.empty() && proc.history.front().timestamp < cutoff) {
            proc.history.pop_front();
        }
    }
}

uint64_t TrafficTracker::getTrafficInWindow(uint32_t pid, int windowSeconds, Direction dir) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = processes_.find(pid);
    if (it == processes_.end()) return 0;

    const auto& proc = it->second;
    if (proc.history.empty()) return 0;

    auto currentTime = now();
    auto windowStart = currentTime - std::chrono::seconds(windowSeconds);

    // Find the snapshot closest to windowStart
    uint64_t startVal = 0;
    bool foundStart = false;
    for (const auto& snap : proc.history) {
        if (snap.timestamp >= windowStart) {
            startVal = (dir == Direction::Upload) ? snap.totalSent : snap.totalRecv;
            foundStart = true;
            break;
        }
    }

    if (!foundStart) {
        // Use the oldest snapshot
        const auto& oldest = proc.history.front();
        startVal = (dir == Direction::Upload) ? oldest.totalSent : oldest.totalRecv;
    }

    uint64_t endVal = (dir == Direction::Upload) ? proc.totalSent : proc.totalRecv;
    return endVal >= startVal ? endVal - startVal : 0;
}

std::map<uint32_t, ProcessTrafficInfo> TrafficTracker::getSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<uint32_t, ProcessTrafficInfo> result;
    for (const auto& [pid, proc] : processes_) {
        ProcessTrafficInfo info;
        info.pid = pid;
        info.processName = proc.processName;
        info.processPath = proc.processPath;
        info.totalSent = proc.totalSent;
        info.totalRecv = proc.totalRecv;
        info.sendRate = proc.sendRate;
        info.recvRate = proc.recvRate;
        result[pid] = std::move(info);
    }
    return result;
}

void TrafficTracker::pruneInactive(int inactiveSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto currentTime = now();
    auto cutoff = currentTime - std::chrono::seconds(inactiveSeconds);

    for (auto it = processes_.begin(); it != processes_.end();) {
        const auto& proc = it->second;
        bool inactive = proc.lastActiveTime < cutoff;
        if (inactive) {
            it = processes_.erase(it);
        } else {
            ++it;
        }
    }
}
