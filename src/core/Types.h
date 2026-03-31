#pragma once

#include <cstdint>
#include <string>
#include <chrono>

enum class Direction { Upload, Download };

struct ProcessTrafficInfo {
    uint32_t pid = 0;
    std::wstring processName;
    std::wstring processPath;
    uint64_t totalSent = 0;
    uint64_t totalRecv = 0;
    double sendRate = 0.0;   // bytes/sec
    double recvRate = 0.0;   // bytes/sec
    uint64_t sendLimit = 0;  // bytes/sec, 0 = unlimited
    uint64_t recvLimit = 0;  // bytes/sec, 0 = unlimited
    bool sendBlocked = false;
    bool recvBlocked = false;
};

struct AlertPolicy {
    int id = 0;
    uint32_t pid = 0;
    std::wstring processName;
    uint64_t thresholdBytes = 0;    // e.g., 500 * 1024 * 1024
    int windowSeconds = 0;          // e.g., 600
    Direction direction = Direction::Download;
    uint64_t limitBytesPerSec = 0;  // limit to apply when triggered
    bool triggered = false;
};

struct AlertAction {
    int policyId = 0;
    uint32_t pid = 0;
    Direction direction = Direction::Download;
    uint64_t limitBytesPerSec = 0;
};

struct TrafficEvent {
    uint32_t pid = 0;
    uint32_t bytes = 0;
    Direction direction = Direction::Download;
    std::chrono::steady_clock::time_point timestamp;
};
