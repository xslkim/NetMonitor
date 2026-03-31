#include <gtest/gtest.h>
#include "core/AlertManager.h"

class AlertManagerTest : public ::testing::Test {
protected:
    AlertManager manager;

    AlertPolicy makePolicy(uint32_t pid, uint64_t threshold, int window,
                           Direction dir = Direction::Download,
                           uint64_t limit = 10240) {
        AlertPolicy p;
        p.pid = pid;
        p.processName = L"test.exe";
        p.thresholdBytes = threshold;
        p.windowSeconds = window;
        p.direction = dir;
        p.limitBytesPerSec = limit;
        return p;
    }
};

TEST_F(AlertManagerTest, AddPolicyReturnsUniqueId) {
    int id1 = manager.addPolicy(makePolicy(100, 1000, 60));
    int id2 = manager.addPolicy(makePolicy(200, 2000, 120));
    EXPECT_NE(id1, id2);
}

TEST_F(AlertManagerTest, GetPoliciesReturnsAll) {
    manager.addPolicy(makePolicy(100, 1000, 60));
    manager.addPolicy(makePolicy(200, 2000, 120));
    EXPECT_EQ(manager.getPolicies().size(), 2u);
}

TEST_F(AlertManagerTest, RemovePolicyWorks) {
    int id = manager.addPolicy(makePolicy(100, 1000, 60));
    manager.removePolicy(id);
    EXPECT_EQ(manager.getPolicies().size(), 0u);
}

TEST_F(AlertManagerTest, EvaluateTriggersWhenThresholdExceeded) {
    manager.addPolicy(makePolicy(100, 500'000'000, 600)); // 500MB in 10 min

    auto actions = manager.evaluate([](uint32_t pid, int window, Direction dir) -> uint64_t {
        if (pid == 100 && window == 600 && dir == Direction::Download)
            return 600'000'000; // 600MB > 500MB
        return 0;
    });

    ASSERT_EQ(actions.size(), 1u);
    EXPECT_EQ(actions[0].pid, 100u);
    EXPECT_EQ(actions[0].limitBytesPerSec, 10240u);
}

TEST_F(AlertManagerTest, EvaluateDoesNotTriggerBelowThreshold) {
    manager.addPolicy(makePolicy(100, 500'000'000, 600));

    auto actions = manager.evaluate([](uint32_t pid, int window, Direction dir) -> uint64_t {
        return 100'000'000; // 100MB < 500MB
    });

    EXPECT_TRUE(actions.empty());
}

TEST_F(AlertManagerTest, TriggeredPolicyDoesNotTriggerAgain) {
    manager.addPolicy(makePolicy(100, 1000, 60));

    // First evaluation: trigger
    auto actions1 = manager.evaluate([](uint32_t, int, Direction) -> uint64_t { return 2000; });
    EXPECT_EQ(actions1.size(), 1u);

    // Second evaluation: already triggered, no new action
    auto actions2 = manager.evaluate([](uint32_t, int, Direction) -> uint64_t { return 3000; });
    EXPECT_TRUE(actions2.empty());
}

TEST_F(AlertManagerTest, ResetPolicyAllowsRetrigger) {
    int id = manager.addPolicy(makePolicy(100, 1000, 60));

    manager.evaluate([](uint32_t, int, Direction) -> uint64_t { return 2000; });
    manager.resetPolicy(id);

    auto actions = manager.evaluate([](uint32_t, int, Direction) -> uint64_t { return 2000; });
    EXPECT_EQ(actions.size(), 1u);
}

TEST_F(AlertManagerTest, CallbackInvokedOnTrigger) {
    bool callbackCalled = false;
    uint32_t callbackPid = 0;

    manager.setAlertCallback([&](const AlertPolicy& p, const AlertAction& a) {
        callbackCalled = true;
        callbackPid = a.pid;
    });

    manager.addPolicy(makePolicy(100, 1000, 60));
    manager.evaluate([](uint32_t, int, Direction) -> uint64_t { return 2000; });

    EXPECT_TRUE(callbackCalled);
    EXPECT_EQ(callbackPid, 100u);
}

TEST_F(AlertManagerTest, MultiplePoliciesSameProcess) {
    manager.addPolicy(makePolicy(100, 1000, 60, Direction::Download, 10240));
    manager.addPolicy(makePolicy(100, 500, 60, Direction::Upload, 5120));

    auto actions = manager.evaluate([](uint32_t pid, int window, Direction dir) -> uint64_t {
        if (dir == Direction::Download) return 2000;
        if (dir == Direction::Upload) return 600;
        return 0;
    });

    ASSERT_EQ(actions.size(), 2u);
}

TEST_F(AlertManagerTest, ClearPoliciesRemovesAll) {
    manager.addPolicy(makePolicy(100, 1000, 60));
    manager.addPolicy(makePolicy(200, 2000, 120));
    manager.clearPolicies();
    EXPECT_TRUE(manager.getPolicies().empty());
}
