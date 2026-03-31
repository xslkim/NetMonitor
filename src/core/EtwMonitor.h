#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <functional>
#include <atomic>
#include <thread>
#include <string>

#include "core/Types.h"

class EtwMonitor {
public:
    using TrafficCallback = std::function<void(uint32_t pid, uint32_t bytes, Direction dir)>;

    EtwMonitor();
    ~EtwMonitor();

    // Start monitoring. Returns true on success.
    bool start();
    void stop();
    bool isRunning() const { return running_; }

    void setCallback(TrafficCallback cb) { callback_ = std::move(cb); }

    std::wstring getLastError() const { return lastError_; }

private:
    static void WINAPI EventRecordCallback(PEVENT_RECORD pEvent);
    void processEvent(PEVENT_RECORD pEvent);

    TrafficCallback callback_;
    std::atomic<bool> running_{false};
    std::thread processThread_;

    TRACEHANDLE sessionHandle_ = INVALID_PROCESSTRACE_HANDLE;
    TRACEHANDLE traceHandle_ = INVALID_PROCESSTRACE_HANDLE;

    std::wstring lastError_;

    static const GUID TcpIpGuid_;
    static const GUID UdpIpGuid_;

    // Per-instance pointer for static callback
    static EtwMonitor* instance_;
};
