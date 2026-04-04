#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <thread>
#include <functional>

#include "core/Types.h"
#include "core/TokenBucket.h"

// ── Packet scheduler (testable without WinDivert) ────────────────────────────

struct QueuedPacket {
    std::vector<uint8_t> data;
    std::vector<uint8_t> addr;  // serialised WINDIVERT_ADDRESS
    uint32_t pid;
    Direction direction;
};

// Decides whether a packet should pass, be queued, or be dropped.
// This class is the testable core; DivertLimiter wraps it with I/O.
enum class PacketAction { Pass, Queue, Drop };

class PacketScheduler {
public:
    static constexpr size_t MAX_QUEUE_PACKETS = 2000;
    static constexpr size_t MAX_QUEUE_BYTES   = 3 * 1024 * 1024;

    void setLimit(uint32_t pid, Direction dir, uint64_t bytesPerSec);
    void removeLimit(uint32_t pid, Direction dir);
    void removeAllLimits(uint32_t pid);
    void clearAllLimits();

    bool hasLimit(uint32_t pid, Direction dir) const;
    uint64_t getLimit(uint32_t pid, Direction dir) const;

    // Decide what to do with an incoming packet.
    PacketAction classify(uint32_t pid, Direction dir, size_t packetSize);

    // Enqueue a packet (caller already got PacketAction::Queue).
    void enqueue(QueuedPacket pkt);

    // Try to drain queued packets that the token buckets now allow.
    // Returns packets that should be reinjected.
    std::vector<QueuedPacket> drain();

    // Flush all queued packets for a PID (used when removing limits).
    std::vector<QueuedPacket> flushQueue(uint32_t pid);

    // Flush everything.
    std::vector<QueuedPacket> flushAll();

    // Quick check: are there any active limits at all?
    bool hasAnyLimit() const;

    mutable std::mutex mutex_;   // public so DivertLimiter can lock around classify+enqueue

private:
    struct PerProcess {
        TokenBucket sendBucket;
        TokenBucket recvBucket;
        uint64_t sendLimit = 0;
        uint64_t recvLimit = 0;
        std::deque<QueuedPacket> sendQueue;
        std::deque<QueuedPacket> recvQueue;
        size_t sendQueueBytes = 0;
        size_t recvQueueBytes = 0;
    };

    static uint64_t burstFor(uint64_t rate) {
        uint64_t b = rate / 10;
        return b > 1500 ? b : 1500;
    }

    // Must be called with mutex_ held.
    PerProcess& getOrCreate(uint32_t pid);

    std::unordered_map<uint32_t, PerProcess> procs_;
};

// ── Flow tracker (maps 5-tuple → PID) ───────────────────────────────────────

struct FlowKey {
    uint8_t  protocol;
    uint32_t localAddr[4];
    uint16_t localPort;
    uint32_t remoteAddr[4];
    uint16_t remotePort;

    bool operator==(const FlowKey& o) const;
};

struct FlowKeyHash {
    size_t operator()(const FlowKey& k) const;
};

class FlowTracker {
public:
    void addFlow(const FlowKey& key, uint32_t pid);
    void removeFlow(const FlowKey& key);
    // Full 5-tuple lookup; for UDP also falls back to local-only match (remote=0).
    uint32_t lookupPid(const FlowKey& key) const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<FlowKey, uint32_t, FlowKeyHash> flows_;
};

// ── DivertLimiter (main class) ──────────────────────────────────────────────

class DivertLimiter {
public:
    DivertLimiter();
    ~DivertLimiter();

    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }

    void setLimit(uint32_t pid, Direction dir, uint64_t bytesPerSec);
    void removeLimit(uint32_t pid, Direction dir);
    void removeAllLimits(uint32_t pid);
    void clearAllLimits();

    bool hasLimit(uint32_t pid, Direction dir) const;

    std::wstring getLastError() const;

private:
    void flowLoop();
    void captureLoop();
    void pacerLoop();
    void populateExistingFlows(uint32_t pid);

    void reinject(const QueuedPacket& pkt);
    void reinjectBatch(std::vector<QueuedPacket>& pkts);

    std::atomic<bool> running_{false};
    HANDLE netHandle_  = INVALID_HANDLE_VALUE;
    HANDLE flowHandle_ = INVALID_HANDLE_VALUE;

    std::thread flowThread_;
    std::thread captureThread_;
    std::thread pacerThread_;

    FlowTracker flowTracker_;
    PacketScheduler scheduler_;

    mutable std::mutex errorMutex_;
    std::wstring lastError_;

    void setError(const std::wstring& msg);
};
