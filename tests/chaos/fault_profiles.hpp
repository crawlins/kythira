// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <fiu-control.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace kythira::chaos {

// Process-wide record of the fault points the profiles below have enabled
// since the last forget_fault_points() (clear_all_faults() calls it at the
// start of every scenario). Requirement 5.5 wants a violation diagnostic to
// name the Fault_Points that were active, and libfiu has no API to list the
// enabled points, so the profiles report here instead.
//
// A point stays in the record after its profile lifts it, marked "lifted":
// most assertions run after the profile's scope closes, and a violation is
// usually caused by a fault that has already ended. A one-shot point
// (enable_once) reads "enabled" until its profile disables it, even after
// libfiu has auto-disabled it on first fire.
namespace detail {
struct fault_point_record {
    std::mutex mutex;
    std::map<std::string, bool> points;  // name -> currently enabled
};

inline auto fault_point_record_instance() -> fault_point_record& {
    static fault_point_record record;
    return record;
}
}  // namespace detail

inline void note_fault_point(const std::string& name, bool enabled) {
    auto& record = detail::fault_point_record_instance();
    std::lock_guard lock(record.mutex);
    record.points[name] = enabled;
}

inline void forget_fault_points() {
    auto& record = detail::fault_point_record_instance();
    std::lock_guard lock(record.mutex);
    record.points.clear();
}

// "fault points: a (enabled), b (lifted)", or "fault points: none".
inline auto describe_fault_points() -> std::string {
    auto& record = detail::fault_point_record_instance();
    std::lock_guard lock(record.mutex);
    if (record.points.empty()) {
        return "fault points: none";
    }
    std::string out = "fault points: ";
    bool first = true;
    for (const auto& [name, enabled] : record.points) {
        if (!first) {
            out += ", ";
        }
        first = false;
        out += name;
        out += enabled ? " (enabled)" : " (lifted)";
    }
    return out;
}

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
            note_fault_point(name, false);
        }
        _active_points.clear();
    }

protected:
    fault_profile() = default;

    void enable_always(const char* name) {
        fiu_enable(name, 1, nullptr, 0);
        _active_points.emplace_back(name);
        note_fault_point(name, true);
    }

    void enable_random(const char* name, double probability) {
        fiu_enable_random(name, 1, nullptr, 0, static_cast<float>(probability));
        _active_points.emplace_back(name);
        note_fault_point(name, true);
    }

    void enable_once(const char* name) {
        fiu_enable(name, 1, nullptr, FIU_ONETIME);
        _active_points.emplace_back(name);
        note_fault_point(name, true);
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
