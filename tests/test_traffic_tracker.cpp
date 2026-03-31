#include <gtest/gtest.h>
#include "core/TrafficTracker.h"

using namespace std::chrono_literals;

class TrafficTrackerTest : public ::testing::Test {
protected:
    void SetUp() override {
        tracker = std::make_unique<TrafficTracker>(600);
        baseTime = TrafficTracker::Clock::now();
        tracker->setTimeForTest(baseTime);
    }

    std::unique_ptr<TrafficTracker> tracker;
    TrafficTracker::TimePoint baseTime;
};

TEST_F(TrafficTrackerTest, AddTrafficIncrementsTotal) {
    tracker->addTraffic(100, 500, Direction::Download);
    tracker->addTraffic(100, 300, Direction::Upload);
    tracker->update();

    auto snap = tracker->getSnapshot();
    ASSERT_EQ(snap.count(100), 1u);
    EXPECT_EQ(snap[100].totalRecv, 500u);
    EXPECT_EQ(snap[100].totalSent, 300u);
}

TEST_F(TrafficTrackerTest, MultipleAddsAccumulate) {
    tracker->addTraffic(100, 500, Direction::Download);
    tracker->addTraffic(100, 200, Direction::Download);
    tracker->addTraffic(100, 100, Direction::Download);
    tracker->update();

    auto snap = tracker->getSnapshot();
    EXPECT_EQ(snap[100].totalRecv, 800u);
}

TEST_F(TrafficTrackerTest, RateCalculation) {
    tracker->addTraffic(100, 1000, Direction::Download);
    tracker->update(); // first snapshot

    tracker->advanceTimeForTest(1000ms);
    tracker->addTraffic(100, 2000, Direction::Download);
    tracker->update(); // second snapshot, rate = 2000 bytes / 1.0s

    auto snap = tracker->getSnapshot();
    EXPECT_NEAR(snap[100].recvRate, 2000.0, 1.0);
}

TEST_F(TrafficTrackerTest, SeparateProcesses) {
    tracker->addTraffic(100, 500, Direction::Download);
    tracker->addTraffic(200, 300, Direction::Upload);
    tracker->update();

    auto snap = tracker->getSnapshot();
    ASSERT_EQ(snap.size(), 2u);
    EXPECT_EQ(snap[100].totalRecv, 500u);
    EXPECT_EQ(snap[200].totalSent, 300u);
}

TEST_F(TrafficTrackerTest, TrafficInWindow) {
    // Simulate 10 seconds of traffic
    for (int i = 0; i < 10; i++) {
        tracker->addTraffic(100, 100, Direction::Download); // 100 bytes/sec
        tracker->update();
        tracker->advanceTimeForTest(1000ms);
    }

    // Last 5 seconds should have ~500 bytes
    uint64_t traffic = tracker->getTrafficInWindow(100, 5, Direction::Download);
    EXPECT_GE(traffic, 400u);
    EXPECT_LE(traffic, 600u);
}

TEST_F(TrafficTrackerTest, TrafficInWindowFullRange) {
    tracker->addTraffic(100, 1000, Direction::Download);
    tracker->update();

    tracker->advanceTimeForTest(5000ms);
    tracker->addTraffic(100, 2000, Direction::Download);
    tracker->update();

    // Window measures delta from first snapshot in window to current total
    // First snapshot has totalRecv=1000, current total=3000, delta=2000
    uint64_t traffic = tracker->getTrafficInWindow(100, 10, Direction::Download);
    EXPECT_EQ(traffic, 2000u);

    // Short window (3s): only the snapshot at t=5s is in range,
    // no traffic occurred after that snapshot, so delta = 0
    uint64_t recentTraffic = tracker->getTrafficInWindow(100, 3, Direction::Download);
    EXPECT_EQ(recentTraffic, 0u);
}

TEST_F(TrafficTrackerTest, UnknownProcessReturnsZero) {
    uint64_t traffic = tracker->getTrafficInWindow(999, 10, Direction::Download);
    EXPECT_EQ(traffic, 0u);
}

TEST_F(TrafficTrackerTest, NameResolverIsCalled) {
    tracker->setNameResolver([](uint32_t pid) -> std::pair<std::wstring, std::wstring> {
        return {L"test.exe", L"C:\\test.exe"};
    });

    tracker->addTraffic(100, 500, Direction::Download);
    tracker->update();

    auto snap = tracker->getSnapshot();
    EXPECT_EQ(snap[100].processName, L"test.exe");
    EXPECT_EQ(snap[100].processPath, L"C:\\test.exe");
}

TEST_F(TrafficTrackerTest, PruneInactiveRemovesOldProcesses) {
    tracker->addTraffic(100, 500, Direction::Download);
    tracker->update();

    tracker->advanceTimeForTest(60000ms); // 60 seconds later
    tracker->update(); // rates will be ~0

    tracker->pruneInactive(30);

    auto snap = tracker->getSnapshot();
    EXPECT_EQ(snap.size(), 0u);
}
