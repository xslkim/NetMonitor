#include "core/DriverClient.h"

#include <vector>

#include "shared/DriverProtocol.h"

namespace {
std::wstring formatSystemMessage(DWORD errorCode) {
    LPWSTR buffer = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD size = FormatMessageW(flags,
                                nullptr,
                                errorCode,
                                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                reinterpret_cast<LPWSTR>(&buffer),
                                0,
                                nullptr);
    std::wstring message = (size > 0 && buffer != nullptr) ? std::wstring(buffer, size) : L"unknown error";
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}
}

DriverClient::~DriverClient() {
    cleanup();
}

bool DriverClient::init() {
    if (isConnected()) {
        return true;
    }

    device_ = CreateFileW(NETMONITOR_DOS_DEVICE_NAME,
                          GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr,
                          OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL,
                          nullptr);
    if (device_ == INVALID_HANDLE_VALUE) {
        setLastErrorMessage(L"无法连接驱动", GetLastError());
        return false;
    }

    lastError_.clear();
    return true;
}

void DriverClient::cleanup() {
    if (device_ != INVALID_HANDLE_VALUE) {
        CloseHandle(device_);
        device_ = INVALID_HANDLE_VALUE;
    }
}

bool DriverClient::setRateLimit(uint32_t pid, Direction direction, uint64_t bytesPerSecond) {
    if (!isConnected() && !init()) {
        return false;
    }

    NetMonitorSetRateLimitRequest request{};
    request.pid = pid;
    request.direction = toDriverDirection(direction);
    request.bytesPerSecond = bytesPerSecond;

    return sendIoctl(IOCTL_NETMONITOR_SET_RATE_LIMIT,
                     &request,
                     sizeof(request),
                     nullptr,
                     0);
}

bool DriverClient::removeRateLimit(uint32_t pid, Direction direction) {
    if (!isConnected() && !init()) {
        return false;
    }

    NetMonitorRemoveRateLimitRequest request{};
    request.pid = pid;
    request.direction = toDriverDirection(direction);

    return sendIoctl(IOCTL_NETMONITOR_REMOVE_RATE_LIMIT,
                     &request,
                     sizeof(request),
                     nullptr,
                     0);
}

bool DriverClient::clearLimits() {
    if (!isConnected() && !init()) {
        return false;
    }

    return sendIoctl(IOCTL_NETMONITOR_CLEAR_LIMITS,
                     nullptr,
                     0,
                     nullptr,
                     0);
}

bool DriverClient::queryRateLimit(uint32_t pid, Direction direction, uint64_t& bytesPerSecond, bool& active) {
    if (!isConnected() && !init()) {
        return false;
    }

    NetMonitorQueryLimitRequest request{};
    request.pid = pid;
    request.direction = toDriverDirection(direction);

    NetMonitorQueryLimitResponse response{};
    DWORD bytesReturned = 0;
    if (!sendIoctl(IOCTL_NETMONITOR_QUERY_LIMIT,
                   &request,
                   sizeof(request),
                   &response,
                   sizeof(response),
                   &bytesReturned)) {
        return false;
    }

    if (bytesReturned < sizeof(response)) {
        lastError_ = L"驱动返回了不完整的查询结果";
        return false;
    }

    bytesPerSecond = response.bytesPerSecond;
    active = response.active != 0;
    return true;
}

bool DriverClient::sendIoctl(DWORD controlCode,
                             void* inBuffer,
                             DWORD inBufferSize,
                             void* outBuffer,
                             DWORD outBufferSize,
                             DWORD* bytesReturned) {
    DWORD localBytesReturned = 0;
    BOOL ok = DeviceIoControl(device_,
                              controlCode,
                              inBuffer,
                              inBufferSize,
                              outBuffer,
                              outBufferSize,
                              bytesReturned != nullptr ? bytesReturned : &localBytesReturned,
                              nullptr);
    if (!ok) {
        setLastErrorMessage(L"驱动 IOCTL 调用失败", GetLastError());
        return false;
    }

    lastError_.clear();
    return true;
}

void DriverClient::setLastErrorMessage(const std::wstring& prefix, DWORD errorCode) {
    lastError_ = prefix + L": " + formatSystemMessage(errorCode) + L" (" + std::to_wstring(errorCode) + L")";
}

uint8_t DriverClient::toDriverDirection(Direction direction) {
    return direction == Direction::Upload
    ? static_cast<uint8_t>(NmRateLimitDirectionUpload)
    : static_cast<uint8_t>(NmRateLimitDirectionDownload);
}
