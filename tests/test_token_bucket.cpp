#include <gtest/gtest.h>
#include "core/TokenBucket.h"

using namespace std::chrono_literals;

class TokenBucketTest : public ::testing::Test {
protected:
    void SetUp() override {
        bucket = std::make_unique<TokenBucket>(1000, 1000); // 1000 bytes/sec, burst 1000
        bucket->setTimeForTest(TokenBucket::Clock::now());
    }
    std::unique_ptr<TokenBucket> bucket;
};

TEST_F(TokenBucketTest, InitialTokensEqualBurst) {
    EXPECT_EQ(bucket->available(), 1000);
}

TEST_F(TokenBucketTest, ConsumeReducesTokens) {
    EXPECT_TRUE(bucket->tryConsume(300));
    EXPECT_EQ(bucket->available(), 700);
}

TEST_F(TokenBucketTest, CannotConsumeMoreThanAvailable) {
    EXPECT_FALSE(bucket->tryConsume(1500));
    EXPECT_EQ(bucket->available(), 1000); // unchanged
}

TEST_F(TokenBucketTest, TokensRefillOverTime) {
    EXPECT_TRUE(bucket->tryConsume(1000)); // drain
    EXPECT_EQ(bucket->available(), 0);

    bucket->advanceTimeForTest(500ms); // 0.5s at 1000/s = 500 tokens
    EXPECT_EQ(bucket->available(), 500);
}

TEST_F(TokenBucketTest, TokensDoNotExceedBurst) {
    bucket->advanceTimeForTest(5000ms); // wait 5 seconds
    EXPECT_EQ(bucket->available(), 1000); // capped at burst
}

TEST_F(TokenBucketTest, PartialConsumptionAndRefill) {
    EXPECT_TRUE(bucket->tryConsume(800));
    bucket->advanceTimeForTest(200ms); // +200 tokens
    EXPECT_EQ(bucket->available(), 400); // 200 + 200
}

TEST_F(TokenBucketTest, SetRateChangesRefillSpeed) {
    bucket->tryConsume(1000);
    bucket->setRate(2000, 2000); // double the rate
    bucket->advanceTimeForTest(500ms); // 0.5s at 2000/s = 1000 tokens
    EXPECT_EQ(bucket->available(), 1000);
}

TEST_F(TokenBucketTest, ResetFillsToBurst) {
    bucket->tryConsume(500);
    bucket->reset();
    EXPECT_EQ(bucket->available(), 1000);
}

TEST_F(TokenBucketTest, ZeroRateMeansNoRefill) {
    TokenBucket zeroBucket(0, 100);
    zeroBucket.setTimeForTest(TokenBucket::Clock::now());
    zeroBucket.tryConsume(100);
    zeroBucket.advanceTimeForTest(1000ms);
    EXPECT_EQ(zeroBucket.available(), 0);
}

TEST_F(TokenBucketTest, ExactConsumption) {
    EXPECT_TRUE(bucket->tryConsume(1000));
    EXPECT_EQ(bucket->available(), 0);
    EXPECT_FALSE(bucket->tryConsume(1));
}
