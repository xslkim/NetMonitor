#pragma once

#include <cstdint>
#include <chrono>
#include <mutex>

class TokenBucket {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    TokenBucket() = default;
    TokenBucket(uint64_t ratePerSec, uint64_t burstSize);

    bool tryConsume(uint64_t tokens);
    uint64_t available() const;
    void setRate(uint64_t ratePerSec, uint64_t burstSize);
    uint64_t getRate() const { return rate_; }
    uint64_t getBurst() const { return burst_; }
    void reset();

    // Test support: injectable time
    void setTimeForTest(TimePoint tp);
    void advanceTimeForTest(std::chrono::milliseconds ms);

private:
    void refill();
    TimePoint now() const;

    uint64_t rate_ = 0;
    uint64_t burst_ = 0;
    double tokens_ = 0.0;
    TimePoint lastRefill_{};
    mutable std::mutex mutex_;

    bool useTestTime_ = false;
    TimePoint testTime_{};
};
