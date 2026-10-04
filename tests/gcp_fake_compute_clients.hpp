// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file gcp_fake_compute_clients.hpp
/// @brief Hand-written in-memory test doubles for the three Compute Engine
///        connections the GCP quorum managers use (Requirement 23 AC 3).
///
/// Each fake derives from google-cloud-cpp's generated `*Connection` interface
/// and overrides only the calls the managers make; every other call keeps the
/// base class's `kUnimplemented` default, so a manager that starts using a new
/// API fails loudly here rather than silently passing. A real client is built
/// over each fake and handed to the managers' client-injection constructors.
///
/// The fakes model just enough Compute Engine behaviour to drive the managers:
/// zone-scoped instances with labels and a status, the one `labels.K = "V"`
/// filter shape the managers send, MIGs with a target size and autohealing
/// policies, and zone operations that are `DONE` unless a test scripts
/// otherwise. Call counters let tests assert what was (or was not) called.

#include <google/cloud/compute/instance_group_managers/v1/instance_group_managers_client.h>
#include <google/cloud/compute/instance_group_managers/v1/instance_group_managers_connection.h>
#include <google/cloud/compute/instances/v1/instances_client.h>
#include <google/cloud/compute/instances/v1/instances_connection.h>
#include <google/cloud/compute/zone_operations/v1/zone_operations_client.h>
#include <google/cloud/compute/zone_operations/v1/zone_operations_connection.h>
#include <google/cloud/future.h>
#include <google/cloud/status_or.h>
#include <google/cloud/stream_range.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace kythira::test::gcp_fakes {

namespace cv1 = google::cloud::cpp::compute::v1;
namespace inst_v1 = google::cloud::cpp::compute::instances::v1;
namespace igm_v1 = google::cloud::cpp::compute::instance_group_managers::v1;
namespace zo_v1 = google::cloud::cpp::compute::zone_operations::v1;

/// A zone operation in @p status ("PENDING", "RUNNING" or "DONE").
inline auto make_operation(std::string name, std::string status = "DONE") -> cv1::Operation {
    cv1::Operation op;
    op.set_name(std::move(name));
    op.set_status(std::move(status));
    return op;
}

/// A `DONE` zone operation carrying one GCP-reported error.
inline auto make_failed_operation(std::string name, const std::string& code,
                                  const std::string& message) -> cv1::Operation {
    auto op = make_operation(std::move(name));
    auto* err = op.mutable_error()->add_errors();
    err->set_code(code);
    err->set_message(message);
    return op;
}

inline auto ready(google::cloud::StatusOr<cv1::Operation> op)
    -> google::cloud::future<google::cloud::StatusOr<cv1::Operation>> {
    return google::cloud::make_ready_future(std::move(op));
}

inline auto not_found(const std::string& what) -> google::cloud::Status {
    return google::cloud::Status(google::cloud::StatusCode::kNotFound, what + " not found");
}

/// Builds a `StreamRange` that yields @p values, then ends with @p final_status.
template<typename T>
auto make_stream_range(std::vector<T> values, google::cloud::Status final_status = {})
    -> google::cloud::StreamRange<T> {
    return google::cloud::internal::MakeStreamRange<T>(
        [v = std::move(values), i = std::size_t{0},
         s = std::move(final_status)]() mutable -> absl::variant<google::cloud::Status, T> {
            if (i == v.size()) {
                return s;
            }
            return v[i++];
        });
}

/// Parses the one filter shape the managers send, `labels.KEY = "VALUE"`.
inline auto parse_label_filter(const std::string& filter)
    -> std::optional<std::pair<std::string, std::string>> {
    static const std::regex re(R"re(^labels\.([a-z0-9_-]+) = "([^"]*)"$)re");
    std::smatch m;
    if (!std::regex_match(filter, m, re)) {
        return std::nullopt;
    }
    return std::pair{m[1].str(), m[2].str()};
}

// ============================================================================
// zoneOperations
// ============================================================================

/// Answers `zoneOperations.get` with a `DONE`, error-free operation unless
/// `on_get` is set, in which case it decides the response.
class fake_zone_operations_connection
    : public google::cloud::compute_zone_operations_v1::ZoneOperationsConnection {
public:
    std::function<google::cloud::StatusOr<cv1::Operation>(const zo_v1::GetOperationRequest&)>
        on_get;
    int get_calls = 0;
    std::vector<std::string> polled;  ///< Operation names, in poll order.

    auto GetOperation(const zo_v1::GetOperationRequest& request)
        -> google::cloud::StatusOr<cv1::Operation> override {
        ++get_calls;
        polled.push_back(request.operation());
        if (on_get) {
            return on_get(request);
        }
        return make_operation(request.operation());
    }
};

// ============================================================================
// instances
// ============================================================================

/// An in-memory set of zone-scoped instances. `insert` creates the instance
/// already `RUNNING` with the next `10.0.0.N` internal address.
class fake_instances_connection : public google::cloud::compute_instances_v1::InstancesConnection {
public:
    using InstancesConnection::DeleteInstance;
    using InstancesConnection::InsertInstance;
    using InstancesConnection::SetLabels;

    std::vector<cv1::Instance> instances;
    /// When set, `instances.list` yields this error after any matches.
    std::optional<google::cloud::Status> list_error;
    int list_calls = 0;
    int insert_calls = 0;
    int delete_calls = 0;
    int set_labels_calls = 0;

    /// Adds an instance in @p zone and returns it for further tweaking.
    auto add(const std::string& zone, const std::string& name, const std::string& status,
             std::map<std::string, std::string> labels = {}) -> cv1::Instance& {
        cv1::Instance inst;
        inst.set_name(name);
        inst.set_zone(zone);
        inst.set_status(status);
        inst.set_self_link("https://www.googleapis.com/compute/v1/projects/test-project/zones/" +
                           zone + "/instances/" + name);
        inst.add_network_interfaces()->set_network_ip("10.0.0." + std::to_string(++_next_ip));
        for (auto& [k, v] : labels) {
            (*inst.mutable_labels())[k] = v;
        }
        instances.push_back(std::move(inst));
        return instances.back();
    }

    auto find(const std::string& zone, const std::string& name) -> cv1::Instance* {
        auto it = std::find_if(instances.begin(), instances.end(),
                               [&](const auto& i) { return i.zone() == zone && i.name() == name; });
        return it == instances.end() ? nullptr : &*it;
    }

    auto ListInstances(inst_v1::ListInstancesRequest request)
        -> google::cloud::StreamRange<cv1::Instance> override {
        ++list_calls;
        auto filter = parse_label_filter(request.filter());
        std::vector<cv1::Instance> out;
        for (const auto& i : instances) {
            if (i.zone() != request.zone()) {
                continue;
            }
            if (filter) {
                auto it = i.labels().find(filter->first);
                if (it == i.labels().end() || it->second != filter->second) {
                    continue;
                }
            }
            out.push_back(i);
        }
        return make_stream_range(std::move(out), list_error.value_or(google::cloud::Status{}));
    }

    auto GetInstance(const inst_v1::GetInstanceRequest& request)
        -> google::cloud::StatusOr<cv1::Instance> override {
        if (auto* i = find(request.zone(), request.instance())) {
            return *i;
        }
        return not_found("instance " + request.instance());
    }

    auto InsertInstance(const inst_v1::InsertInstanceRequest& request)
        -> google::cloud::future<google::cloud::StatusOr<cv1::Operation>> override {
        ++insert_calls;
        const auto& res = request.instance_resource();
        if (find(request.zone(), res.name()) != nullptr) {
            return ready(google::cloud::Status(google::cloud::StatusCode::kAlreadyExists,
                                               res.name() + " already exists"));
        }
        auto& inst = add(request.zone(), res.name(), "RUNNING");
        *inst.mutable_labels() = res.labels();
        return ready(make_operation("op-insert-" + res.name(), "RUNNING"));
    }

    auto DeleteInstance(const inst_v1::DeleteInstanceRequest& request)
        -> google::cloud::future<google::cloud::StatusOr<cv1::Operation>> override {
        ++delete_calls;
        auto* i = find(request.zone(), request.instance());
        if (i == nullptr) {
            return ready(not_found("instance " + request.instance()));
        }
        instances.erase(instances.begin() + (i - instances.data()));
        return ready(make_operation("op-delete-" + request.instance(), "RUNNING"));
    }

    auto SetLabels(const inst_v1::SetLabelsRequest& request)
        -> google::cloud::future<google::cloud::StatusOr<cv1::Operation>> override {
        ++set_labels_calls;
        auto* i = find(request.zone(), request.instance());
        if (i == nullptr) {
            return ready(not_found("instance " + request.instance()));
        }
        *i->mutable_labels() = request.instances_set_labels_request_resource().labels();
        return ready(make_operation("op-set-labels-" + request.instance(), "RUNNING"));
    }

private:
    int _next_ip = 0;
};

// ============================================================================
// instanceGroupManagers
// ============================================================================

/// An in-memory set of zonal MIGs keyed by name. Resizing changes only the
/// recorded target size; tests that need the resulting instance add it to a
/// `fake_instances_connection` themselves, or model the MIG's reaction with
/// `on_resize`.
///
/// `managed` is what `listManagedInstances` reports per MIG (empty unless a
/// test fills it). `deleteInstances` removes a listed member and lowers the
/// target size, as the real call does.
class fake_instance_group_managers_connection
    : public google::cloud::compute_instance_group_managers_v1::InstanceGroupManagersConnection {
public:
    using InstanceGroupManagersConnection::DeleteInstances;
    using InstanceGroupManagersConnection::Resize;

    std::map<std::string, cv1::InstanceGroupManager> migs;
    std::map<std::string, std::vector<cv1::ManagedInstance>> managed;
    /// Called after `resize` records the new size, with (MIG, old, new).
    std::function<void(const std::string&, std::int32_t, std::int32_t)> on_resize;
    /// Every instance URL `deleteInstances` was asked for, in order.
    std::vector<std::string> deleted;
    /// Whether every `deleteInstances` call set `skipInstancesOnValidationError`.
    bool every_delete_skipped_validation_errors = true;
    int get_calls = 0;
    int resize_calls = 0;
    int list_managed_calls = 0;
    int delete_instances_calls = 0;

    /// Adds a MIG named @p name; `autohealing` gives it an autohealing policy.
    auto add(const std::string& name, std::int32_t target_size = 0, bool autohealing = false)
        -> cv1::InstanceGroupManager& {
        cv1::InstanceGroupManager mig;
        mig.set_name(name);
        mig.set_target_size(target_size);
        if (autohealing) {
            auto* policy = mig.add_auto_healing_policies();
            policy->set_health_check("global/healthChecks/kythira-hc");
            policy->set_initial_delay_sec(300);
        }
        return migs[name] = std::move(mig);
    }

    auto GetInstanceGroupManager(const igm_v1::GetInstanceGroupManagerRequest& request)
        -> google::cloud::StatusOr<cv1::InstanceGroupManager> override {
        ++get_calls;
        auto it = migs.find(request.instance_group_manager());
        if (it == migs.end()) {
            return not_found("instance group manager " + request.instance_group_manager());
        }
        return it->second;
    }

    auto Resize(const igm_v1::ResizeRequest& request)
        -> google::cloud::future<google::cloud::StatusOr<cv1::Operation>> override {
        ++resize_calls;
        auto it = migs.find(request.instance_group_manager());
        if (it == migs.end()) {
            return ready(not_found("instance group manager " + request.instance_group_manager()));
        }
        const auto old_size = it->second.target_size();
        it->second.set_target_size(request.size());
        if (on_resize) {
            on_resize(request.instance_group_manager(), old_size, request.size());
        }
        return ready(make_operation("op-resize-" + request.instance_group_manager(), "RUNNING"));
    }

    auto ListManagedInstances(const igm_v1::ListManagedInstancesRequest& request)
        -> google::cloud::StatusOr<
            cv1::InstanceGroupManagersListManagedInstancesResponse> override {
        ++list_managed_calls;
        if (migs.find(request.instance_group_manager()) == migs.end()) {
            return not_found("instance group manager " + request.instance_group_manager());
        }
        cv1::InstanceGroupManagersListManagedInstancesResponse response;
        if (auto m = managed.find(request.instance_group_manager()); m != managed.end()) {
            for (const auto& mi : m->second) {
                *response.add_managed_instances() = mi;
            }
        }
        return response;
    }

    auto DeleteInstances(const igm_v1::DeleteInstancesRequest& request)
        -> google::cloud::future<google::cloud::StatusOr<cv1::Operation>> override {
        ++delete_instances_calls;
        auto it = migs.find(request.instance_group_manager());
        if (it == migs.end()) {
            return ready(not_found("instance group manager " + request.instance_group_manager()));
        }
        const auto& body = request.instance_group_managers_delete_instances_request_resource();
        every_delete_skipped_validation_errors =
            every_delete_skipped_validation_errors && body.skip_instances_on_validation_error();
        auto& members = managed[request.instance_group_manager()];
        for (const auto& url : body.instances()) {
            deleted.push_back(url);
            auto m = std::find_if(members.begin(), members.end(),
                                  [&](const auto& mi) { return mi.instance() == url; });
            if (m != members.end()) {
                members.erase(m);
                it->second.set_target_size(it->second.target_size() - 1);
            }
        }
        return ready(
            make_operation("op-delete-instances-" + request.instance_group_manager(), "RUNNING"));
    }
};

// ============================================================================
// Client bundle
// ============================================================================

/// The three fakes plus real clients built over them. The fakes stay owned
/// (shared) here so a test can script and inspect them after handing the
/// clients to a manager.
struct fake_compute {
    std::shared_ptr<fake_instances_connection> instances =
        std::make_shared<fake_instances_connection>();
    std::shared_ptr<fake_instance_group_managers_connection> migs =
        std::make_shared<fake_instance_group_managers_connection>();
    std::shared_ptr<fake_zone_operations_connection> zone_ops =
        std::make_shared<fake_zone_operations_connection>();

    [[nodiscard]] auto instances_client() const
        -> google::cloud::compute_instances_v1::InstancesClient {
        return google::cloud::compute_instances_v1::InstancesClient(instances);
    }
    [[nodiscard]] auto migs_client() const
        -> google::cloud::compute_instance_group_managers_v1::InstanceGroupManagersClient {
        return google::cloud::compute_instance_group_managers_v1::InstanceGroupManagersClient(migs);
    }
    [[nodiscard]] auto zone_operations_client() const
        -> google::cloud::compute_zone_operations_v1::ZoneOperationsClient {
        return google::cloud::compute_zone_operations_v1::ZoneOperationsClient(zone_ops);
    }
};

}  // namespace kythira::test::gcp_fakes
