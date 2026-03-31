#include "core/EtwMonitor.h"
#include <vector>
#include <cstring>

#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "advapi32.lib")

// SystemTraceControlGuid - required for NT Kernel Logger
static const GUID s_SystemTraceControlGuid =
    {0x9e814aad, 0x3204, 0x11d2, {0x9a, 0x82, 0x00, 0x60, 0x08, 0xa8, 0x69, 0x39}};

// TcpIp event class GUID
const GUID EtwMonitor::TcpIpGuid_ =
    {0x9A280AC0, 0xC8E0, 0x11D1, {0x84, 0xE2, 0x00, 0xC0, 0x4F, 0xB9, 0x98, 0xA2}};

// UdpIp event class GUID
const GUID EtwMonitor::UdpIpGuid_ =
    {0xBF3A50C5, 0xA9C9, 0x4988, {0xA0, 0x05, 0x2D, 0xF0, 0xB7, 0xC8, 0x0F, 0x80}};

// NT Kernel Logger requires this exact session name
static const wchar_t* KERNEL_SESSION_NAME = KERNEL_LOGGER_NAMEW;

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

static size_t calcBufSize() {
    return sizeof(EVENT_TRACE_PROPERTIES)
         + (wcslen(KERNEL_SESSION_NAME) + 1) * sizeof(wchar_t)
         + sizeof(wchar_t) * 2; // extra padding for LogFileName
}

static EVENT_TRACE_PROPERTIES* makeProps(std::vector<BYTE>& buf) {
    size_t sz = calcBufSize();
    buf.assign(sz, 0);
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
    p->Wnode.BufferSize = static_cast<ULONG>(sz);
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return p;
}

bool EtwMonitor::start() {
    if (running_) return true;
    instance_ = this;

    // Stop any previous NT Kernel Logger session
    {
        std::vector<BYTE> buf;
        auto* props = makeProps(buf);
        ControlTraceW(0, KERNEL_SESSION_NAME, props, EVENT_TRACE_CONTROL_STOP);
    }

    // Allocate and fill properties for new session
    std::vector<BYTE> propBuf;
    auto* props = makeProps(propBuf);

    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1; // QPC clock
    props->Wnode.Guid = s_SystemTraceControlGuid;
    props->EnableFlags = EVENT_TRACE_FLAG_NETWORK_TCPIP;
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    props->BufferSize = 64;        // 64 KB per buffer
    props->MinimumBuffers = 4;
    props->MaximumBuffers = 64;
    props->FlushTimer = 1;         // flush every 1 second

    ULONG status = StartTraceW(&sessionHandle_, KERNEL_SESSION_NAME, props);
    if (status != ERROR_SUCCESS) {
        lastError_ = L"StartTrace failed: " + std::to_wstring(status);
        if (status == ERROR_ACCESS_DENIED) {
            lastError_ += L" (需要管理员权限)";
        } else if (status == ERROR_INVALID_PARAMETER) {
            lastError_ += L" (参数错误)";
        } else if (status == ERROR_ALREADY_EXISTS) {
            lastError_ += L" (会话已存在，请关闭其他 ETW 工具后重试)";
        }
        return false;
    }

    // Open trace for consuming
    EVENT_TRACE_LOGFILEW logFile = {};
    logFile.LoggerName = const_cast<LPWSTR>(KERNEL_SESSION_NAME);
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
        std::vector<BYTE> buf;
        auto* props = makeProps(buf);
        ControlTraceW(sessionHandle_, nullptr, props, EVENT_TRACE_CONTROL_STOP);
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
