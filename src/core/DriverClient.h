#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

#include "core/Types.h"

class DriverClient {
public:
    DriverClient() = default;
    ~DriverClient();

    bool init();
    void cleanup();

    bool isConnected() const { return device_ != INVALID_HANDLE_VALUE; }

    bool setRateLimit(uint32_t pid, Direction direction, uint64_t bytesPerSecond);
    bool removeRateLimit(uint32_t pid, Direction direction);
    bool clearLimits();
    bool queryRateLimit(uint32_t pid, Direction direction, uint64_t& bytesPerSecond, bool& active);

    std::wstring getLastError() const { return lastError_; }

private:
    bool sendIoctl(DWORD controlCode,
                   void* inBuffer,
                   DWORD inBufferSize,
                   void* outBuffer,
                   DWORD outBufferSize,
                   DWORD* bytesReturned = nullptr);
    void setLastErrorMessage(const std::wstring& prefix, DWORD errorCode);

    static uint8_t toDriverDirection(Direction direction);

    HANDLE device_ = INVALID_HANDLE_VALUE;
    std::wstring lastError_;
};
