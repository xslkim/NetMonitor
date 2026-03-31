#pragma once

#include "core/Types.h"
#include <vector>
#include <functional>
#include <mutex>
#include <string>

class AlertManager {
public:
    using TrafficQueryFn = std::function<uint64_t(uint32_t pid, int windowSeconds, Direction dir)>;
    using AlertCallback = std::function<void(const AlertPolicy& policy, const AlertAction& action)>;

    int addPolicy(AlertPolicy policy);
    void removePolicy(int id);
    void clearPolicies();

    std::vector<AlertPolicy> getPolicies() const;

    // Evaluate all policies. queryFn provides traffic data.
    // Returns list of newly triggered actions.
    std::vector<AlertAction> evaluate(TrafficQueryFn queryFn);

    // Called when an alert triggers. Optional callback.
    void setAlertCallback(AlertCallback cb) { callback_ = std::move(cb); }

    // Reset triggered state for a policy
    void resetPolicy(int id);

private:
    std::vector<AlertPolicy> policies_;
    int nextId_ = 1;
    mutable std::mutex mutex_;
    AlertCallback callback_;
};
