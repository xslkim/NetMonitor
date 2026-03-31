#include "core/AlertManager.h"
#include <algorithm>

int AlertManager::addPolicy(AlertPolicy policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    policy.id = nextId_++;
    policies_.push_back(std::move(policy));
    return policies_.back().id;
}

void AlertManager::removePolicy(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    policies_.erase(
        std::remove_if(policies_.begin(), policies_.end(),
                        [id](const AlertPolicy& p) { return p.id == id; }),
        policies_.end());
}

void AlertManager::clearPolicies() {
    std::lock_guard<std::mutex> lock(mutex_);
    policies_.clear();
}

std::vector<AlertPolicy> AlertManager::getPolicies() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return policies_;
}

std::vector<AlertAction> AlertManager::evaluate(TrafficQueryFn queryFn) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AlertAction> actions;

    for (auto& policy : policies_) {
        if (policy.triggered) continue;

        uint64_t traffic = queryFn(policy.pid, policy.windowSeconds, policy.direction);
        if (traffic >= policy.thresholdBytes) {
            policy.triggered = true;
            AlertAction action;
            action.policyId = policy.id;
            action.pid = policy.pid;
            action.direction = policy.direction;
            action.limitBytesPerSec = policy.limitBytesPerSec;
            actions.push_back(action);

            if (callback_) {
                callback_(policy, action);
            }
        }
    }

    return actions;
}

void AlertManager::resetPolicy(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& policy : policies_) {
        if (policy.id == id) {
            policy.triggered = false;
            break;
        }
    }
}
