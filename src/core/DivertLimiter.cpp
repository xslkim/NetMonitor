#include "core/DivertLimiter.h"
#include <windivert.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <algorithm>
#include <cstring>

// ══════════════════════════════════════════════════════════════════════════════
// FlowKey
// ══════════════════════════════════════════════════════════════════════════════

bool FlowKey::operator==(const FlowKey& o) const {
    return protocol == o.protocol &&
           localPort == o.localPort && remotePort == o.remotePort &&
           std::memcmp(localAddr, o.localAddr, sizeof(localAddr)) == 0 &&
           std::memcmp(remoteAddr, o.remoteAddr, sizeof(remoteAddr)) == 0;
}

size_t FlowKeyHash::operator()(const FlowKey& k) const {
    // FNV-1a over the raw bytes
    size_t h = 14695981039346656037ULL;
    auto feed = [&](const void* p, size_t n) {
        auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; i++) {
            h ^= b[i];
            h *= 1099511628211ULL;
        }
    };
    feed(&k.protocol, 1);
    feed(k.localAddr, sizeof(k.localAddr));
    feed(&k.localPort, 2);
    feed(k.remoteAddr, sizeof(k.remoteAddr));
    feed(&k.remotePort, 2);
    return h;
}

// ══════════════════════════════════════════════════════════════════════════════
// FlowTracker
// ══════════════════════════════════════════════════════════════════════════════

void FlowTracker::addFlow(const FlowKey& key, uint32_t pid) {
    std::unique_lock lock(mutex_);
    flows_[key] = pid;
}

void FlowTracker::removeFlow(const FlowKey& key) {
    std::unique_lock lock(mutex_);
    flows_.erase(key);
}

uint32_t FlowTracker::lookupPid(const FlowKey& key) const {
    std::shared_lock lock(mutex_);
    auto it = flows_.find(key);
    if (it != flows_.end()) return it->second;

    // For UDP: also try wildcard-remote key (populated from system table for
    // pre-existing connections where we only know local addr/port).
    if (key.protocol == IPPROTO_UDP) {
        FlowKey partial = key;
        std::memset(partial.remoteAddr, 0, sizeof(partial.remoteAddr));
        partial.remotePort = 0;
        auto it2 = flows_.find(partial);
        if (it2 != flows_.end()) return it2->second;
    }
    return 0;
}

// ══════════════════════════════════════════════════════════════════════════════
// PacketScheduler
// ══════════════════════════════════════════════════════════════════════════════

PacketScheduler::PerProcess& PacketScheduler::getOrCreate(uint32_t pid) {
    auto it = procs_.find(pid);
    if (it != procs_.end()) return it->second;
    return procs_[pid];
}

void PacketScheduler::setLimit(uint32_t pid, Direction dir, uint64_t bytesPerSec) {
    std::lock_guard lock(mutex_);
    auto& pp = getOrCreate(pid);
    if (dir == Direction::Upload) {
        pp.sendLimit = bytesPerSec;
        pp.sendBucket.setRate(bytesPerSec, burstFor(bytesPerSec));
    } else {
        pp.recvLimit = bytesPerSec;
        pp.recvBucket.setRate(bytesPerSec, burstFor(bytesPerSec));
    }
}

void PacketScheduler::removeLimit(uint32_t pid, Direction dir) {
    std::lock_guard lock(mutex_);
    auto it = procs_.find(pid);
    if (it == procs_.end()) return;
    auto& pp = it->second;
    if (dir == Direction::Upload) {
        pp.sendLimit = 0;
    } else {
        pp.recvLimit = 0;
    }
    if (pp.sendLimit == 0 && pp.recvLimit == 0) {
        procs_.erase(it);
    }
}

void PacketScheduler::removeAllLimits(uint32_t pid) {
    std::lock_guard lock(mutex_);
    procs_.erase(pid);
}

void PacketScheduler::clearAllLimits() {
    std::lock_guard lock(mutex_);
    procs_.clear();
}

bool PacketScheduler::hasLimit(uint32_t pid, Direction dir) const {
    std::lock_guard lock(mutex_);
    auto it = procs_.find(pid);
    if (it == procs_.end()) return false;
    return dir == Direction::Upload ? it->second.sendLimit > 0 : it->second.recvLimit > 0;
}

uint64_t PacketScheduler::getLimit(uint32_t pid, Direction dir) const {
    std::lock_guard lock(mutex_);
    auto it = procs_.find(pid);
    if (it == procs_.end()) return 0;
    return dir == Direction::Upload ? it->second.sendLimit : it->second.recvLimit;
}

bool PacketScheduler::hasAnyLimit() const {
    std::lock_guard lock(mutex_);
    return !procs_.empty();
}

PacketAction PacketScheduler::classify(uint32_t pid, Direction dir, size_t packetSize) {
    // Must be called with mutex_ held.
    auto it = procs_.find(pid);
    if (it == procs_.end()) return PacketAction::Pass;

    auto& pp = it->second;
    auto& bucket = (dir == Direction::Upload) ? pp.sendBucket : pp.recvBucket;
    uint64_t limit = (dir == Direction::Upload) ? pp.sendLimit : pp.recvLimit;

    if (limit == 0) return PacketAction::Pass;

    if (bucket.tryConsume(static_cast<uint64_t>(packetSize))) {
        return PacketAction::Pass;
    }

    // Check queue limits
    auto& queue = (dir == Direction::Upload) ? pp.sendQueue : pp.recvQueue;
    size_t& qBytes = (dir == Direction::Upload) ? pp.sendQueueBytes : pp.recvQueueBytes;
    if (queue.size() >= MAX_QUEUE_PACKETS || qBytes >= MAX_QUEUE_BYTES) {
        return PacketAction::Drop;
    }
    return PacketAction::Queue;
}

void PacketScheduler::enqueue(QueuedPacket pkt) {
    // Must be called with mutex_ held.
    auto it = procs_.find(pkt.pid);
    if (it == procs_.end()) return;  // limit was removed between classify and enqueue

    auto& pp = it->second;
    auto& queue = (pkt.direction == Direction::Upload) ? pp.sendQueue : pp.recvQueue;
    size_t& qBytes = (pkt.direction == Direction::Upload) ? pp.sendQueueBytes : pp.recvQueueBytes;

    qBytes += pkt.data.size();
    queue.push_back(std::move(pkt));
}

std::vector<QueuedPacket> PacketScheduler::drain() {
    std::lock_guard lock(mutex_);
    std::vector<QueuedPacket> result;

    for (auto& [pid, pp] : procs_) {
        auto drainQueue = [&](std::deque<QueuedPacket>& queue, TokenBucket& bucket,
                              size_t& qBytes) {
            while (!queue.empty()) {
                auto& front = queue.front();
                if (!bucket.tryConsume(static_cast<uint64_t>(front.data.size()))) break;
                qBytes -= front.data.size();
                result.push_back(std::move(front));
                queue.pop_front();
            }
        };
        if (pp.sendLimit > 0)
            drainQueue(pp.sendQueue, pp.sendBucket, pp.sendQueueBytes);
        if (pp.recvLimit > 0)
            drainQueue(pp.recvQueue, pp.recvBucket, pp.recvQueueBytes);
    }
    return result;
}

std::vector<QueuedPacket> PacketScheduler::flushQueue(uint32_t pid) {
    std::lock_guard lock(mutex_);
    std::vector<QueuedPacket> result;
    auto it = procs_.find(pid);
    if (it == procs_.end()) return result;

    auto& pp = it->second;
    for (auto& pkt : pp.sendQueue) result.push_back(std::move(pkt));
    for (auto& pkt : pp.recvQueue) result.push_back(std::move(pkt));
    pp.sendQueue.clear();
    pp.recvQueue.clear();
    pp.sendQueueBytes = 0;
    pp.recvQueueBytes = 0;
    return result;
}

std::vector<QueuedPacket> PacketScheduler::flushAll() {
    std::lock_guard lock(mutex_);
    std::vector<QueuedPacket> result;
    for (auto& [pid, pp] : procs_) {
        for (auto& pkt : pp.sendQueue) result.push_back(std::move(pkt));
        for (auto& pkt : pp.recvQueue) result.push_back(std::move(pkt));
        pp.sendQueue.clear();
        pp.recvQueue.clear();
        pp.sendQueueBytes = 0;
        pp.recvQueueBytes = 0;
    }
    return result;
}

// ══════════════════════════════════════════════════════════════════════════════
// DivertLimiter
// ══════════════════════════════════════════════════════════════════════════════

DivertLimiter::DivertLimiter() = default;

DivertLimiter::~DivertLimiter() {
    stop();
}

void DivertLimiter::setError(const std::wstring& msg) {
    std::lock_guard lock(errorMutex_);
    lastError_ = msg;
}

std::wstring DivertLimiter::getLastError() const {
    std::lock_guard lock(errorMutex_);
    return lastError_;
}

static std::wstring winErrorString(DWORD err) {
    wchar_t buf[256];
    swprintf_s(buf, L"错误码 %lu", err);
    switch (err) {
    case ERROR_FILE_NOT_FOUND:  return std::wstring(buf) + L" - 找不到 WinDivert64.sys";
    case ERROR_ACCESS_DENIED:   return std::wstring(buf) + L" - 需要管理员权限";
    case 577:                   return std::wstring(buf) + L" - 驱动签名被阻止 (Secure Boot)";
    default:                    return buf;
    }
}

bool DivertLimiter::start() {
    if (running_.load()) return true;

    // Open flow handle (PID tracking, does not intercept packets)
    flowHandle_ = WinDivertOpen("true", WINDIVERT_LAYER_FLOW, 0,
                                WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY);
    if (flowHandle_ == INVALID_HANDLE_VALUE) {
        setError(L"Flow handle 打开失败: " + winErrorString(GetLastError()));
        return false;
    }

    // Open network handle (intercepts TCP/UDP, excluding loopback)
    netHandle_ = WinDivertOpen("!loopback and (tcp or udp)",
                               WINDIVERT_LAYER_NETWORK, 0, 0);
    if (netHandle_ == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        WinDivertClose(flowHandle_);
        flowHandle_ = INVALID_HANDLE_VALUE;
        setError(L"Network handle 打开失败: " + winErrorString(err));
        return false;
    }

    // Increase queue capacity for better throughput
    WinDivertSetParam(netHandle_, WINDIVERT_PARAM_QUEUE_LENGTH, 16384);
    WinDivertSetParam(netHandle_, WINDIVERT_PARAM_QUEUE_TIME, 2000);
    WinDivertSetParam(netHandle_, WINDIVERT_PARAM_QUEUE_SIZE, 33554432); // 32 MB

    running_.store(true);
    flowThread_    = std::thread(&DivertLimiter::flowLoop, this);
    captureThread_ = std::thread(&DivertLimiter::captureLoop, this);
    pacerThread_   = std::thread(&DivertLimiter::pacerLoop, this);

    return true;
}

void DivertLimiter::stop() {
    if (!running_.load()) return;
    running_.store(false);

    // Shutdown handles to unblock Recv calls
    if (netHandle_ != INVALID_HANDLE_VALUE)
        WinDivertShutdown(netHandle_, WINDIVERT_SHUTDOWN_BOTH);
    if (flowHandle_ != INVALID_HANDLE_VALUE)
        WinDivertShutdown(flowHandle_, WINDIVERT_SHUTDOWN_BOTH);

    if (captureThread_.joinable()) captureThread_.join();
    if (flowThread_.joinable())    flowThread_.join();
    if (pacerThread_.joinable())   pacerThread_.join();

    if (netHandle_ != INVALID_HANDLE_VALUE) {
        WinDivertClose(netHandle_);
        netHandle_ = INVALID_HANDLE_VALUE;
    }
    if (flowHandle_ != INVALID_HANDLE_VALUE) {
        WinDivertClose(flowHandle_);
        flowHandle_ = INVALID_HANDLE_VALUE;
    }
}

// ── Limit management (forwarded to scheduler, plus flush) ────────────────────

// Populate the flow table with connections that already existed when setLimit is
// called (WinDivert's flow layer only captures new connections going forward).
void DivertLimiter::populateExistingFlows(uint32_t pid) {
    DWORD size = 0;

    // ── TCP v4 ────────────────────────────────────────────────────────────────
    size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size > 0) {
        std::vector<uint8_t> buf(size + 256);
        size = static_cast<DWORD>(buf.size());
        if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET,
                                TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < table->dwNumEntries; i++) {
                if (table->table[i].dwOwningPid != pid) continue;
                FlowKey key{};
                key.protocol      = IPPROTO_TCP;
                key.localAddr[0]  = table->table[i].dwLocalAddr;
                key.localPort     = ntohs(static_cast<u_short>(table->table[i].dwLocalPort));
                key.remoteAddr[0] = table->table[i].dwRemoteAddr;
                key.remotePort    = ntohs(static_cast<u_short>(table->table[i].dwRemotePort));
                flowTracker_.addFlow(key, pid);
            }
        }
    }

    // ── TCP v6 ────────────────────────────────────────────────────────────────
    size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size > 0) {
        std::vector<uint8_t> buf(size + 256);
        size = static_cast<DWORD>(buf.size());
        if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET6,
                                TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            auto* table = reinterpret_cast<MIB_TCP6TABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < table->dwNumEntries; i++) {
                if (table->table[i].dwOwningPid != pid) continue;
                FlowKey key{};
                key.protocol   = IPPROTO_TCP;
                std::memcpy(key.localAddr,  table->table[i].ucLocalAddr,  16);
                std::memcpy(key.remoteAddr, table->table[i].ucRemoteAddr, 16);
                key.localPort  = ntohs(static_cast<u_short>(table->table[i].dwLocalPort));
                key.remotePort = ntohs(static_cast<u_short>(table->table[i].dwRemotePort));
                flowTracker_.addFlow(key, pid);
            }
        }
    }

    // ── UDP v4 (no remote addr available; stored as wildcard-remote key) ──────
    size = 0;
    GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    if (size > 0) {
        std::vector<uint8_t> buf(size + 256);
        size = static_cast<DWORD>(buf.size());
        if (GetExtendedUdpTable(buf.data(), &size, FALSE, AF_INET,
                                UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            auto* table = reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < table->dwNumEntries; i++) {
                if (table->table[i].dwOwningPid != pid) continue;
                FlowKey key{};
                key.protocol     = IPPROTO_UDP;
                key.localAddr[0] = table->table[i].dwLocalAddr;
                key.localPort    = ntohs(static_cast<u_short>(table->table[i].dwLocalPort));
                // remoteAddr/remotePort left as 0 (wildcard); lookupPid falls back to this
                flowTracker_.addFlow(key, pid);
            }
        }
    }

    // ── UDP v6 ────────────────────────────────────────────────────────────────
    size = 0;
    GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0);
    if (size > 0) {
        std::vector<uint8_t> buf(size + 256);
        size = static_cast<DWORD>(buf.size());
        if (GetExtendedUdpTable(buf.data(), &size, FALSE, AF_INET6,
                                UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            auto* table = reinterpret_cast<MIB_UDP6TABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < table->dwNumEntries; i++) {
                if (table->table[i].dwOwningPid != pid) continue;
                FlowKey key{};
                key.protocol  = IPPROTO_UDP;
                std::memcpy(key.localAddr, table->table[i].ucLocalAddr, 16);
                key.localPort = ntohs(static_cast<u_short>(table->table[i].dwLocalPort));
                flowTracker_.addFlow(key, pid);
            }
        }
    }
}

void DivertLimiter::setLimit(uint32_t pid, Direction dir, uint64_t bytesPerSec) {
    populateExistingFlows(pid);  // capture pre-existing connections before enabling limit
    scheduler_.setLimit(pid, dir, bytesPerSec);
}

void DivertLimiter::removeLimit(uint32_t pid, Direction dir) {
    // Flush queued packets before removing limit
    auto flushed = scheduler_.flushQueue(pid);
    reinjectBatch(flushed);
    scheduler_.removeLimit(pid, dir);
}

void DivertLimiter::removeAllLimits(uint32_t pid) {
    auto flushed = scheduler_.flushQueue(pid);
    reinjectBatch(flushed);
    scheduler_.removeAllLimits(pid);
}

void DivertLimiter::clearAllLimits() {
    auto flushed = scheduler_.flushAll();
    reinjectBatch(flushed);
    scheduler_.clearAllLimits();
}

bool DivertLimiter::hasLimit(uint32_t pid, Direction dir) const {
    return scheduler_.hasLimit(pid, dir);
}

// ── Reinject helpers ─────────────────────────────────────────────────────────

void DivertLimiter::reinject(const QueuedPacket& pkt) {
    if (netHandle_ == INVALID_HANDLE_VALUE) return;
    auto* addrPtr = reinterpret_cast<const WINDIVERT_ADDRESS*>(pkt.addr.data());
    WinDivertSend(netHandle_, pkt.data.data(),
                  static_cast<UINT>(pkt.data.size()), nullptr, addrPtr);
}

void DivertLimiter::reinjectBatch(std::vector<QueuedPacket>& pkts) {
    for (auto& pkt : pkts) reinject(pkt);
}

// ── Helper: extract FlowKey from IP packet headers ──────────────────────────

static bool extractFlowKey(const uint8_t* packet, UINT len,
                           const WINDIVERT_ADDRESS& addr, FlowKey& key) {
    PWINDIVERT_IPHDR ipHdr = nullptr;
    PWINDIVERT_IPV6HDR ipv6Hdr = nullptr;
    PWINDIVERT_TCPHDR tcpHdr = nullptr;
    PWINDIVERT_UDPHDR udpHdr = nullptr;

    WinDivertHelperParsePacket(
        const_cast<uint8_t*>(packet), len,
        &ipHdr, &ipv6Hdr, nullptr,
        nullptr, nullptr,
        &tcpHdr, &udpHdr,
        nullptr, nullptr, nullptr, nullptr);

    std::memset(&key, 0, sizeof(key));

    if (ipHdr) {
        key.localAddr[0] = addr.Outbound ? ipHdr->SrcAddr : ipHdr->DstAddr;
        key.remoteAddr[0] = addr.Outbound ? ipHdr->DstAddr : ipHdr->SrcAddr;
        key.protocol = ipHdr->Protocol;
    } else if (ipv6Hdr) {
        if (addr.Outbound) {
            std::memcpy(key.localAddr, ipv6Hdr->SrcAddr, 16);
            std::memcpy(key.remoteAddr, ipv6Hdr->DstAddr, 16);
        } else {
            std::memcpy(key.localAddr, ipv6Hdr->DstAddr, 16);
            std::memcpy(key.remoteAddr, ipv6Hdr->SrcAddr, 16);
        }
        key.protocol = ipv6Hdr->NextHdr;
    } else {
        return false;
    }

    if (tcpHdr) {
        key.localPort  = addr.Outbound ? ntohs(tcpHdr->SrcPort) : ntohs(tcpHdr->DstPort);
        key.remotePort = addr.Outbound ? ntohs(tcpHdr->DstPort) : ntohs(tcpHdr->SrcPort);
    } else if (udpHdr) {
        key.localPort  = addr.Outbound ? ntohs(udpHdr->SrcPort) : ntohs(udpHdr->DstPort);
        key.remotePort = addr.Outbound ? ntohs(udpHdr->DstPort) : ntohs(udpHdr->SrcPort);
    } else {
        return false;
    }
    return true;
}

// ── Flow tracking thread ────────────────────────────────────────────────────

void DivertLimiter::flowLoop() {
    WINDIVERT_ADDRESS addr;
    uint8_t dummy[1];

    while (running_.load()) {
        UINT recvLen = 0;
        if (!WinDivertRecv(flowHandle_, dummy, 0, &recvLen, &addr)) {
            if (!running_.load()) break;
            continue;
        }

        FlowKey key;
        std::memset(&key, 0, sizeof(key));
        key.protocol   = addr.Flow.Protocol;
        key.localPort  = addr.Flow.LocalPort;
        key.remotePort = addr.Flow.RemotePort;
        std::memcpy(key.localAddr, addr.Flow.LocalAddr, sizeof(key.localAddr));
        std::memcpy(key.remoteAddr, addr.Flow.RemoteAddr, sizeof(key.remoteAddr));

        if (addr.Event == WINDIVERT_EVENT_FLOW_ESTABLISHED) {
            flowTracker_.addFlow(key, addr.Flow.ProcessId);
        } else if (addr.Event == WINDIVERT_EVENT_FLOW_DELETED) {
            flowTracker_.removeFlow(key);
        }
    }
}

// ── Real-time PID lookup via system TCP/UDP tables ──────────────────────────
// Used as fallback when the flow tracker doesn't have the connection (e.g.
// pre-existing connections, wildcard-bound UDP sockets).

static uint32_t lookupPidFromSystemTable(const FlowKey& key) {
    if (key.protocol == IPPROTO_TCP) {
        // IPv4 TCP – match full 5-tuple
        DWORD size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
        if (size > 0) {
            std::vector<uint8_t> buf(size + 256);
            size = static_cast<DWORD>(buf.size());
            if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET,
                                    TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
                auto* t = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
                for (DWORD i = 0; i < t->dwNumEntries; i++) {
                    auto& r = t->table[i];
                    if (r.dwLocalAddr  == key.localAddr[0]  &&
                        r.dwRemoteAddr == key.remoteAddr[0] &&
                        ntohs(static_cast<u_short>(r.dwLocalPort))  == key.localPort &&
                        ntohs(static_cast<u_short>(r.dwRemotePort)) == key.remotePort) {
                        return r.dwOwningPid;
                    }
                }
            }
        }
        // IPv6 TCP
        size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0);
        if (size > 0) {
            std::vector<uint8_t> buf(size + 256);
            size = static_cast<DWORD>(buf.size());
            if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET6,
                                    TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
                auto* t = reinterpret_cast<MIB_TCP6TABLE_OWNER_PID*>(buf.data());
                for (DWORD i = 0; i < t->dwNumEntries; i++) {
                    auto& r = t->table[i];
                    if (std::memcmp(r.ucLocalAddr, key.localAddr, 16) == 0 &&
                        std::memcmp(r.ucRemoteAddr, key.remoteAddr, 16) == 0 &&
                        ntohs(static_cast<u_short>(r.dwLocalPort))  == key.localPort &&
                        ntohs(static_cast<u_short>(r.dwRemotePort)) == key.remotePort) {
                        return r.dwOwningPid;
                    }
                }
            }
        }
    } else if (key.protocol == IPPROTO_UDP) {
        // IPv4 UDP – match local port; local addr may be 0.0.0.0 (wildcard bind)
        DWORD size = 0;
        GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
        if (size > 0) {
            std::vector<uint8_t> buf(size + 256);
            size = static_cast<DWORD>(buf.size());
            if (GetExtendedUdpTable(buf.data(), &size, FALSE, AF_INET,
                                    UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
                auto* t = reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(buf.data());
                for (DWORD i = 0; i < t->dwNumEntries; i++) {
                    auto& r = t->table[i];
                    if (ntohs(static_cast<u_short>(r.dwLocalPort)) == key.localPort &&
                        (r.dwLocalAddr == key.localAddr[0] || r.dwLocalAddr == 0)) {
                        return r.dwOwningPid;
                    }
                }
            }
        }
        // IPv6 UDP
        size = 0;
        GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0);
        if (size > 0) {
            std::vector<uint8_t> buf(size + 256);
            size = static_cast<DWORD>(buf.size());
            if (GetExtendedUdpTable(buf.data(), &size, FALSE, AF_INET6,
                                    UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
                auto* t = reinterpret_cast<MIB_UDP6TABLE_OWNER_PID*>(buf.data());
                static const uint8_t zeroes[16] = {};
                for (DWORD i = 0; i < t->dwNumEntries; i++) {
                    auto& r = t->table[i];
                    if (ntohs(static_cast<u_short>(r.dwLocalPort)) == key.localPort &&
                        (std::memcmp(r.ucLocalAddr, key.localAddr, 16) == 0 ||
                         std::memcmp(r.ucLocalAddr, zeroes, 16) == 0)) {
                        return r.dwOwningPid;
                    }
                }
            }
        }
    }
    return 0;
}

// ── Capture thread (main packet interception) ───────────────────────────────

void DivertLimiter::captureLoop() {
    constexpr UINT BUF_SIZE = 65535 + 40;  // max IP packet + some headroom
    auto buffer = std::make_unique<uint8_t[]>(BUF_SIZE);
    WINDIVERT_ADDRESS addr;

    while (running_.load()) {
        UINT recvLen = 0;
        if (!WinDivertRecv(netHandle_, buffer.get(), BUF_SIZE, &recvLen, &addr)) {
            if (!running_.load()) break;
            continue;
        }

        // Determine PID via flow tracker
        FlowKey key;
        uint32_t pid = 0;
        bool keyValid = extractFlowKey(buffer.get(), recvLen, addr, key);
        if (keyValid) {
            pid = flowTracker_.lookupPid(key);
        }

        // Fallback: if flow tracker missed this connection (pre-existing, wildcard
        // bind, etc.), query the OS TCP/UDP tables directly.  Only bother when we
        // have active limits – otherwise just fast-path.
        if (pid == 0 && keyValid && scheduler_.hasAnyLimit()) {
            pid = lookupPidFromSystemTable(key);
            if (pid != 0) {
                // Cache so subsequent packets of the same flow are fast.
                flowTracker_.addFlow(key, pid);
            }
        }

        Direction dir = addr.Outbound ? Direction::Upload : Direction::Download;

        // Fast path: unknown PID or no limit → reinject immediately
        if (pid == 0) {
            WinDivertSend(netHandle_, buffer.get(), recvLen, nullptr, &addr);
            continue;
        }

        {
            std::lock_guard lock(scheduler_.mutex_);
            PacketAction action = scheduler_.classify(pid, dir, recvLen);

            switch (action) {
            case PacketAction::Pass:
                WinDivertSend(netHandle_, buffer.get(), recvLen, nullptr, &addr);
                break;
            case PacketAction::Queue: {
                QueuedPacket pkt;
                pkt.data.assign(buffer.get(), buffer.get() + recvLen);
                pkt.addr.resize(sizeof(addr));
                std::memcpy(pkt.addr.data(), &addr, sizeof(addr));
                pkt.pid = pid;
                pkt.direction = dir;
                scheduler_.enqueue(std::move(pkt));
                break;
            }
            case PacketAction::Drop:
                // Silently drop. TCP will retransmit; UDP loss is acceptable.
                break;
            }
        }
    }
}

// ── Pacer thread (drains queued packets at controlled rate) ─────────────────

void DivertLimiter::pacerLoop() {
    while (running_.load()) {
        Sleep(10);  // ~10 ms interval
        if (!running_.load()) break;

        auto packets = scheduler_.drain();
        for (auto& pkt : packets) {
            reinject(pkt);
        }
    }
}
