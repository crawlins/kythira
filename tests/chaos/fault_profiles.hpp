#pragma once

#include <fiu-control.h>
#include <string>
#include <vector>

namespace kythira::chaos {

// RAII base for fault profiles: enables fault points on construction,
// disables all of them on destruction or on an explicit disable() call.
class fault_profile {
public:
    virtual ~fault_profile() { disable(); }
    fault_profile(const fault_profile&) = delete;
    fault_profile& operator=(const fault_profile&) = delete;

    void disable() {
        for (const auto& name : _active_points) {
            fiu_disable(name.c_str());
        }
        _active_points.clear();
    }

protected:
    fault_profile() = default;

    void enable_always(const char* name) {
        fiu_enable(name, 1, nullptr, 0);
        _active_points.emplace_back(name);
    }

    void enable_random(const char* name, double probability) {
        fiu_enable_random(name, 1, nullptr, 0, static_cast<float>(probability));
        _active_points.emplace_back(name);
    }

    void enable_once(const char* name) {
        fiu_enable(name, 1, nullptr, FIU_ONETIME);
        _active_points.emplace_back(name);
    }

private:
    std::vector<std::string> _active_points;
};

// All sends from the affected node fail — models a hard network partition.
class network_partition_profile : public fault_profile {
public:
    network_partition_profile() {
        enable_always("raft/network/send_request_vote");
        enable_always("raft/network/send_append_entries");
    }
};

// Intermittent write errors — models disk degradation (default 10%).
class disk_degradation_profile : public fault_profile {
public:
    explicit disk_degradation_profile(double failure_probability = 0.10) {
        enable_random("raft/persistence/append_log_entry", failure_probability);
        enable_random("raft/persistence/save_current_term", failure_probability);
    }
};

// Single crash-on-persist event: the next save_current_term fails, then auto-disables.
class leader_crash_profile : public fault_profile {
public:
    leader_crash_profile() { enable_once("raft/persistence/save_current_term"); }
};

// Unreliable state machine — models a degraded application layer (default 5%).
class state_machine_fault_profile : public fault_profile {
public:
    explicit state_machine_fault_profile(double failure_probability = 0.05) {
        enable_random("raft/state_machine/apply", failure_probability);
    }
};

}  // namespace kythira::chaos
