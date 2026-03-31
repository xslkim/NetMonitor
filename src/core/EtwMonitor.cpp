#include "core/EtwMonitor.h"
#include <vector>
#include <cstring>

#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "advapi32.lib")

// Define SystemTraceControlGuid if not provided by the linker
static const GUID s_SystemTraceControlGuid =
    {0x9e814aad, 0x3204, 0x11d2, {0x9a, 0x82, 0x00, 0x60, 0x08, 0xa8, 0x69, 0x39}};

// TcpIp event class GUID
const GUID EtwMonitor::TcpIpGuid_ =
    {0x9A280AC0, 0xC8E0, 0x11D1, {0x84, 0xE2, 0x00, 0xC0, 0x4F, 0xB9, 0x98, 0xA2}};

// UdpIp event class GUID
const GUID EtwMonitor::UdpIpGuid_ =
    {0xBF3A50C5, 0xA9C9, 0x4988, {0xA0, 0x05, 0x2D, 0xF0, 0xB7, 0xC8, 0x0F, 0x80}};

static const wchar_t* SESSION_NAME = L"NetMonitorEtwSession";

EtwMonitor* EtwMonitor::instance_ = nullptr;

// Event data layout for TcpIp/UdpIp Send/Recv (IPv4)
#pragma pack(push, 1)
struct TcpIpEventData {
    ULONG  PID;
    ULONG  size;
    ULONG  daddr;
    ULONG  saddr;
    USHORT dport;
    USHORT sport;
};
#pragma pack(pop)

EtwMonitor::EtwMonitor() = default;

EtwMonitor::~EtwMonitor() {
    stop();
}

bool EtwMonitor::start() {
    if (running_) return true;
    instance_ = this;

    // Stop any previous session with the same name
    {
        size_t bufSize = sizeof(EVENT_TRACE_PROPERTIES) + (wcslen(SESSION_NAME) + 1) * sizeof(wchar_t);
        std::vector<BYTE> buf(bufSize, 0);
        auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
        props->Wnode.BufferSize = static_cast<ULONG>(bufSize);
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTrace(0, SESSION_NAME, props, EVENT_TRACE_CONTROL_STOP);
    }

    // Allocate properties
    size_t bufSize = sizeof(EVENT_TRACE_PROPERTIES) + (wcslen(SESSION_NAME) + 1) * sizeof(wchar_t);
    std::vector<BYTE> propBuf(bufSize, 0);
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propBuf.data());

    props->Wnode.BufferSize = static_cast<ULONG>(bufSize);
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1; // QPC clock
    props->Wnode.Guid = s_SystemTraceControlGuid;
    props->EnableFlags = EVENT_TRACE_FLAG_NETWORK_TCPIP;
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    ULONG status = StartTraceW(&sessionHandle_, SESSION_NAME, props);
    if (status != ERROR_SUCCESS) {
        lastError_ = L"StartTrace failed: " + std::to_wstring(status);
        if (status == ERROR_ACCESS_DENIED) {
            lastError_ += L" (需要管理员权限)";
        }
        return false;
    }

    // Open trace for consuming
    EVENT_TRACE_LOGFILEW logFile = {};
    logFile.LoggerName = const_cast<LPWSTR>(SESSION_NAME);
    logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logFile.EventRecordCallback = EventRecordCallback;

    traceHandle_ = OpenTraceW(&logFile);
    if (traceHandle_ == INVALID_PROCESSTRACE_HANDLE) {
        lastError_ = L"OpenTrace failed: " + std::to_wstring(GetLastError());
        stop();
        return false;
    }

    running_ = true;

    // ProcessTrace blocks, run in a thread
    processThread_ = std::thread([this]() {
        ProcessTrace(&traceHandle_, 1, nullptr, nullptr);
        running_ = false;
    });

    return true;
}

void EtwMonitor::stop() {
    if (sessionHandle_ != INVALID_PROCESSTRACE_HANDLE) {
        size_t bufSize = sizeof(EVENT_TRACE_PROPERTIES) + (wcslen(SESSION_NAME) + 1) * sizeof(wchar_t);
        std::vector<BYTE> buf(bufSize, 0);
        auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
        props->Wnode.BufferSize = static_cast<ULONG>(bufSize);
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTrace(sessionHandle_, nullptr, props, EVENT_TRACE_CONTROL_STOP);
        sessionHandle_ = INVALID_PROCESSTRACE_HANDLE;
    }

    if (traceHandle_ != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(traceHandle_);
        traceHandle_ = INVALID_PROCESSTRACE_HANDLE;
    }

    running_ = false;

    if (processThread_.joinable()) {
        processThread_.join();
    }

    instance_ = nullptr;
}

void WINAPI EtwMonitor::EventRecordCallback(PEVENT_RECORD pEvent) {
    if (instance_) {
        instance_->processEvent(pEvent);
    }
}

void EtwMonitor::processEvent(PEVENT_RECORD pEvent) {
    if (!callback_ || !pEvent->UserData) return;

    // Check if this is a TcpIp or UdpIp event
    bool isTcp = IsEqualGUID(pEvent->EventHeader.ProviderId, TcpIpGuid_);
    bool isUdp = IsEqualGUID(pEvent->EventHeader.ProviderId, UdpIpGuid_);
    if (!isTcp && !isUdp) return;

    UCHAR opcode = pEvent->EventHeader.EventDescriptor.Opcode;
    Direction dir;

    // Opcode 10 = Send (IPv4), 11 = Recv (IPv4)
    // Opcode 26 = Send (IPv6), 27 = Recv (IPv6)
    if (opcode == 10 || opcode == 26) {
        dir = Direction::Upload;
    } else if (opcode == 11 || opcode == 27) {
        dir = Direction::Download;
    } else {
        return;
    }

    if (pEvent->UserDataLength < sizeof(TcpIpEventData)) return;

    auto* data = reinterpret_cast<const TcpIpEventData*>(pEvent->UserData);
    if (data->PID == 0 || data->size == 0) return;

    callback_(data->PID, data->size, dir);
}
