#include "core/TokenBucket.h"
#include <algorithm>

TokenBucket::TokenBucket(uint64_t ratePerSec, uint64_t burstSize)
    : rate_(ratePerSec)
    , burst_(burstSize)
    , tokens_(static_cast<double>(burstSize))
    , lastRefill_(Clock::now()) {}

TokenBucket::TimePoint TokenBucket::now() const {
    return useTestTime_ ? testTime_ : Clock::now();
}

void TokenBucket::refill() {
    auto current = now();
    double elapsed = std::chrono::duration<double>(current - lastRefill_).count();
    if (elapsed > 0) {
        tokens_ = std::min(static_cast<double>(burst_),
                           tokens_ + elapsed * static_cast<double>(rate_));
        lastRefill_ = current;
    }
}

bool TokenBucket::tryConsume(uint64_t tokens) {
    std::lock_guard<std::mutex> lock(mutex_);
    refill();
    double needed = static_cast<double>(tokens);
    if (tokens_ >= needed) {
        tokens_ -= needed;
        return true;
    }
    return false;
}

uint64_t TokenBucket::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const_cast<TokenBucket*>(this)->refill();
    return static_cast<uint64_t>(tokens_);
}

void TokenBucket::setRate(uint64_t ratePerSec, uint64_t burstSize) {
    std::lock_guard<std::mutex> lock(mutex_);
    rate_ = ratePerSec;
    burst_ = burstSize;
    tokens_ = std::min(tokens_, static_cast<double>(burst_));
}

void TokenBucket::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    tokens_ = static_cast<double>(burst_);
    lastRefill_ = now();
}

void TokenBucket::setTimeForTest(TimePoint tp) {
    std::lock_guard<std::mutex> lock(mutex_);
    useTestTime_ = true;
    testTime_ = tp;
    lastRefill_ = tp;
}

void TokenBucket::advanceTimeForTest(std::chrono::milliseconds ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    testTime_ += ms;
}
