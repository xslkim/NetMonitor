#include <gtest/gtest.h>
#include "core/DivertLimiter.h"

using namespace std::chrono_literals;

// Tests for PacketScheduler (the testable core, no WinDivert dependency)

class PacketSchedulerTest : public ::testing::Test {
protected:
    PacketScheduler sched;

    QueuedPacket makePacket(uint32_t pid, Direction dir, size_t size) {
        QueuedPacket pkt;
        pkt.data.resize(size, 0xAB);
        pkt.addr.resize(96, 0);  // placeholder addr (tests don't reinject)
        pkt.pid = pid;
        pkt.direction = dir;
        return pkt;
    }
};

TEST_F(PacketSchedulerTest, UnlimitedPidPassesThrough) {
    std::lock_guard lock(sched.mutex_);
    EXPECT_EQ(sched.classify(100, Direction::Upload, 1000), PacketAction::Pass);
    EXPECT_EQ(sched.classify(100, Direction::Download, 1000), PacketAction::Pass);
}

TEST_F(PacketSchedulerTest, LimitedPidWithinBudgetPasses) {
    sched.setLimit(100, Direction::Download, 100000); // 100 KB/s
    std::lock_guard lock(sched.mutex_);
    // Burst = max(100000/10, 1500) = 10000, initial tokens = burst
    EXPECT_EQ(sched.classify(100, Direction::Download, 1000), PacketAction::Pass);
}

TEST_F(PacketSchedulerTest, LimitedPidOverBudgetQueues) {
    sched.setLimit(100, Direction::Download, 1500); // 1500 B/s, burst = 1500
    {
        std::lock_guard lock(sched.mutex_);
        // Consume all initial tokens
        EXPECT_EQ(sched.classify(100, Direction::Download, 1500), PacketAction::Pass);
        // Now over budget
        EXPECT_EQ(sched.classify(100, Direction::Download, 100), PacketAction::Queue);
    }
}

TEST_F(PacketSchedulerTest, QueuedPacketDrainedAfterTokenRefill) {
    sched.setLimit(100, Direction::Download, 10000); // 10 KB/s, burst = 1500
    {
        std::lock_guard lock(sched.mutex_);
        // Exhaust tokens
        sched.classify(100, Direction::Download, 1500);
        // Queue a packet
        EXPECT_EQ(sched.classify(100, Direction::Download, 100), PacketAction::Queue);
        sched.enqueue(makePacket(100, Direction::Download, 100));
    }
    // Wait enough for tokens to refill (10KB/s → 100 bytes in 10ms)
    Sleep(50);
    auto drained = sched.drain();
    EXPECT_GE(drained.size(), 1u);
}

TEST_F(PacketSchedulerTest, RemoveLimitMakesPassThrough) {
    sched.setLimit(100, Direction::Download, 1000);
    EXPECT_TRUE(sched.hasLimit(100, Direction::Download));
    sched.removeLimit(100, Direction::Download);
    EXPECT_FALSE(sched.hasLimit(100, Direction::Download));
    {
        std::lock_guard lock(sched.mutex_);
        EXPECT_EQ(sched.classify(100, Direction::Download, 5000), PacketAction::Pass);
    }
}

TEST_F(PacketSchedulerTest, RemoveAllLimits) {
    sched.setLimit(100, Direction::Download, 1000);
    sched.setLimit(100, Direction::Upload, 2000);
    sched.removeAllLimits(100);
    EXPECT_FALSE(sched.hasLimit(100, Direction::Download));
    EXPECT_FALSE(sched.hasLimit(100, Direction::Upload));
}

TEST_F(PacketSchedulerTest, ClearAllLimits) {
    sched.setLimit(100, Direction::Download, 1000);
    sched.setLimit(200, Direction::Upload, 2000);
    sched.clearAllLimits();
    EXPECT_FALSE(sched.hasLimit(100, Direction::Download));
    EXPECT_FALSE(sched.hasLimit(200, Direction::Upload));
}

TEST_F(PacketSchedulerTest, MultipleProcessesIndependent) {
    sched.setLimit(100, Direction::Download, 1500);
    sched.setLimit(200, Direction::Download, 1500);
    {
        std::lock_guard lock(sched.mutex_);
        // Exhaust PID 100
        EXPECT_EQ(sched.classify(100, Direction::Download, 1500), PacketAction::Pass);
        EXPECT_EQ(sched.classify(100, Direction::Download, 100), PacketAction::Queue);
        // PID 200 still has budget
        EXPECT_EQ(sched.classify(200, Direction::Download, 1500), PacketAction::Pass);
    }
}

TEST_F(PacketSchedulerTest, DirectionIndependence) {
    sched.setLimit(100, Direction::Download, 1500);
    // Upload is not limited
    {
        std::lock_guard lock(sched.mutex_);
        EXPECT_EQ(sched.classify(100, Direction::Upload, 999999), PacketAction::Pass);
        // Download is limited
        EXPECT_EQ(sched.classify(100, Direction::Download, 1500), PacketAction::Pass);
        EXPECT_EQ(sched.classify(100, Direction::Download, 100), PacketAction::Queue);
    }
}

TEST_F(PacketSchedulerTest, FlushQueueReturnsPackets) {
    sched.setLimit(100, Direction::Download, 1500);
    {
        std::lock_guard lock(sched.mutex_);
        sched.classify(100, Direction::Download, 1500); // exhaust
        sched.classify(100, Direction::Download, 200);  // queue
        sched.enqueue(makePacket(100, Direction::Download, 200));
        sched.classify(100, Direction::Download, 300);
        sched.enqueue(makePacket(100, Direction::Download, 300));
    }
    auto flushed = sched.flushQueue(100);
    EXPECT_EQ(flushed.size(), 2u);
}

TEST_F(PacketSchedulerTest, GetLimitReturnsCorrectValue) {
    sched.setLimit(100, Direction::Download, 50000);
    sched.setLimit(100, Direction::Upload, 25000);
    EXPECT_EQ(sched.getLimit(100, Direction::Download), 50000u);
    EXPECT_EQ(sched.getLimit(100, Direction::Upload), 25000u);
    EXPECT_EQ(sched.getLimit(999, Direction::Download), 0u);
}

// FlowTracker tests

class FlowTrackerTest : public ::testing::Test {
protected:
    FlowTracker tracker;

    FlowKey makeKey(uint8_t proto, uint16_t lport, uint16_t rport) {
        FlowKey k{};
        k.protocol = proto;
        k.localPort = lport;
        k.remotePort = rport;
        k.localAddr[0] = 0x0100007F; // 127.0.0.1
        k.remoteAddr[0] = 0x0200007F;
        return k;
    }
};

TEST_F(FlowTrackerTest, AddAndLookup) {
    auto key = makeKey(6, 12345, 80);
    tracker.addFlow(key, 1234);
    EXPECT_EQ(tracker.lookupPid(key), 1234u);
}

TEST_F(FlowTrackerTest, LookupMissReturnsZero) {
    auto key = makeKey(6, 12345, 80);
    EXPECT_EQ(tracker.lookupPid(key), 0u);
}

TEST_F(FlowTrackerTest, RemoveFlow) {
    auto key = makeKey(6, 12345, 80);
    tracker.addFlow(key, 1234);
    tracker.removeFlow(key);
    EXPECT_EQ(tracker.lookupPid(key), 0u);
}

TEST_F(FlowTrackerTest, MultipleFlows) {
    auto k1 = makeKey(6, 10000, 80);
    auto k2 = makeKey(17, 20000, 443);
    tracker.addFlow(k1, 100);
    tracker.addFlow(k2, 200);
    EXPECT_EQ(tracker.lookupPid(k1), 100u);
    EXPECT_EQ(tracker.lookupPid(k2), 200u);
}
