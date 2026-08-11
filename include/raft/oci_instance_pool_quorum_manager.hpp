#pragma once

/// @file oci_instance_pool_quorum_manager.hpp
/// @brief `quorum_manager` backed by an OCI Instance Pool
///        (`.kiro/specs/oci-cloud-provider/`, Requirements 2-10).
///
/// The OCI analogue of `aws_asg_quorum_manager`, and deliberately close to it in
/// shape: grow the pool by one to provision, detach-with-terminate to
/// decommission, and derive nothing about identity from the cloud provider's own
/// resource identifier. That last point is the one structural difference worth
/// stating up front — an OCID's trailing segment is an opaque, variable-length
/// token, so `aws_ec2_quorum_manager`'s "hex-decode the instance ID into a
/// NodeId" trick has no OCI counterpart. NodeIds are assigned here and recorded
/// in a `kythira-node-id` freeform tag, exactly as `aws_asg_quorum_manager`
/// already does for the identical reason.
///
/// **One Instance Pool per Availability Domain.** `UpdateInstancePool`'s `size`
/// is a single pool-wide integer and OCI's own placement algorithm decides which
/// of the pool's placement configurations absorbs new capacity — there is no
/// per-AD count anywhere in the API (confirmed in `spike-notes.md` Finding 2).
/// A manager driving a multi-AD pool therefore could not honour
/// `provision_node(target_group)` at all; it could only grow the pool and hope.
/// So the contract is that each manager instance owns one single-AD pool, and
/// `provision_node` rejects a `target_group` the pool is not configured for
/// rather than silently provisioning into the wrong failure domain. The caller
/// deploys one manager per AD; see `docker/oci_quorum_manager/README.md`.
///
/// Header-only and always compiled — there is no OCI SDK to detect. The kconfig
/// flag `CONFIG_OCI_QUORUM_MANAGER` selects whether the *tests* for it are built,
/// not whether the header works.

#include <raft/fault_injection.hpp>
#include <raft/future_default.hpp>
#include <raft/oci_client_config.hpp>
#include <raft/oci_http_client.hpp>
#include <raft/quorum_management.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <iostream>
#include <map>
#include <optional>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace kythira {

namespace oci_detail {

/// The well-known freeform tag keys (Requirement 4.1).
///
/// Hyphens where the AWS tags use colons: OCI freeform tag keys do not accept
/// `:`. Named constants rather than string literals at each use because the
/// scan side and the write side must agree exactly, and a typo in one of the two
/// produces a manager that tags instances it will never find again.
inline constexpr const char* tag_cluster = "kythira-cluster";
inline constexpr const char* tag_node_id = "kythira-node-id";
inline constexpr const char* tag_group = "kythira-group";
inline constexpr const char* tag_managed_by = "kythira-managed-by";
inline constexpr const char* tag_last_heartbeat = "kythira-last-heartbeat";
inline constexpr const char* managed_by_value = "kythira-oci-instance-pool-quorum-manager";

/// Read a string field, or `""` when it is absent or not a string.
///
/// Absent and wrong-typed collapse into the same answer on purpose: every caller
/// here treats a missing field as "this instance does not tell me that", and a
/// control plane that answered with a number where a string was documented is
/// the same situation from the caller's point of view.
[[nodiscard]] inline auto json_string(const boost::json::value& v, std::string_view key)
    -> std::string {
    const auto* obj = v.if_object();
    if (obj == nullptr) {
        return {};
    }
    const auto* field = obj->if_contains(key);
    if (field == nullptr || !field->is_string()) {
        return {};
    }
    return std::string(field->get_string());
}

/// Read an integral field, or `fallback` when it is absent or not a number.
[[nodiscard]] inline auto json_int(const boost::json::value& v, std::string_view key,
                                   std::int64_t fallback = 0) -> std::int64_t {
    const auto* obj = v.if_object();
    if (obj == nullptr) {
        return fallback;
    }
    const auto* field = obj->if_contains(key);
    if (field == nullptr) {
        return fallback;
    }
    if (field->is_int64()) {
        return field->get_int64();
    }
    if (field->is_uint64()) {
        return static_cast<std::int64_t>(field->get_uint64());
    }
    if (field->is_double()) {
        return static_cast<std::int64_t>(field->get_double());
    }
    return fallback;
}

/// Read a `freeformTags` object into a plain map, skipping non-string values.
[[nodiscard]] inline auto freeform_tags_of(const boost::json::value& v)
    -> std::map<std::string, std::string> {
    std::map<std::string, std::string> out;
    const auto* obj = v.if_object();
    if (obj == nullptr) {
        return out;
    }
    const auto* tags = obj->if_contains("freeformTags");
    if (tags == nullptr) {
        return out;
    }
    const auto* tag_obj = tags->if_object();
    if (tag_obj == nullptr) {
        return out;
    }
    for (const auto& entry : *tag_obj) {
        if (entry.value().is_string()) {
            out.emplace(std::string(entry.key()), std::string(entry.value().get_string()));
        }
    }
    return out;
}

/// Parse an OCI RFC 3339 timestamp (`timeCreated`) into a `time_t`.
///
/// **UTC only.** OCI stamps these with a trailing `Z`, and a string that does
/// not end in one is rejected rather than parsed as if it did: silently reading
/// `+05:00` as UTC would shift an instance's apparent age by five hours, which
/// on the grace-period path is the difference between "still booting" and
/// "unreachable". Rejecting is visible in the caller's fallback; misparsing is
/// not.
[[nodiscard]] inline auto parse_rfc3339_utc(const std::string& stamp)
    -> std::optional<std::time_t> {
    if (stamp.empty() || stamp.back() != 'Z') {
        return std::nullopt;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    // NOLINTNEXTLINE(cert-err34-c) — the field count is checked below.
    if (std::sscanf(stamp.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute,
                    &second) != 6) {
        return std::nullopt;
    }
    std::tm tm_utc{};
    tm_utc.tm_year = year - 1900;
    tm_utc.tm_mon = month - 1;
    tm_utc.tm_mday = day;
    tm_utc.tm_hour = hour;
    tm_utc.tm_min = minute;
    tm_utc.tm_sec = second;
#if defined(_WIN32)
    const std::time_t out = _mkgmtime(&tm_utc);
#else
    const std::time_t out = timegm(&tm_utc);
#endif
    if (out == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }
    return out;
}

/// One instance as `assess_quorum`/`provision_node`/`decommission_node` need it.
struct instance_view {
    std::string id;                                      ///< Instance OCID.
    std::string lifecycle_state;                         ///< `RUNNING`, `TERMINATED`, ...
    std::string availability_domain;                     ///< As reported by `GetInstance`.
    std::string time_created;                            ///< RFC 3339, UTC.
    std::map<std::string, std::string> freeform_tags{};  ///< Full tag map, verbatim.
};

/// Run `work(i)` for every `i` in `[0, count)` across at most `limit` threads.
///
/// A hand-rolled pool rather than an executor because this header must not
/// require one: the only concurrency it needs is Requirement 5.2's "issue the
/// per-instance `GetInstance` calls concurrently, bounded by a small worker
/// pool", and pulling a scheduler into a header that otherwise depends on
/// nothing but `httplib` would be a much larger commitment than that.
///
/// The first exception any worker raises is rethrown on the calling thread once
/// every worker has finished — not the moment it happens. Cancelling the rest
/// early would leave in-flight HTTP requests with nowhere to land, and one
/// throttled `GetInstance` is not a reason to abandon the others.
template<typename Work> auto parallel_for(std::size_t count, std::size_t limit, Work work) -> void {
    if (count == 0) {
        return;
    }
    const std::size_t workers = std::min(count, std::max<std::size_t>(limit, 1));
    std::atomic<std::size_t> next{0};
    std::vector<std::exception_ptr> errors(workers);
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (std::size_t w = 0; w < workers; ++w) {
        threads.emplace_back([&, w] {
            for (;;) {
                const std::size_t i = next.fetch_add(1);
                if (i >= count) {
                    return;
                }
                try {
                    work(i);
                } catch (...) {
                    if (errors[w] == nullptr) {
                        errors[w] = std::current_exception();
                    }
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    for (const auto& err : errors) {
        if (err != nullptr) {
            std::rethrow_exception(err);
        }
    }
}

}  // namespace oci_detail

// ============================================================================
// oci_instance_pool_quorum_manager_config
// ============================================================================

/// @brief Configuration for @ref oci_instance_pool_quorum_manager
///        (Requirement 2.1).
///
/// Shorter than `aws_ec2_quorum_manager_config` by exactly the fields that
/// describe *what to launch* — image, shape, subnet, user-data. Those live in
/// the pre-existing OCI Instance Configuration the pool already references, the
/// same reason `aws_asg_quorum_manager_config` defers them to the ASG's launch
/// template. This manager never calls `CreateInstanceConfiguration` or
/// `CreateInstancePool`.
struct oci_instance_pool_quorum_manager_config {
    /// Credentials, region and signing mode.
    oci_client_config oci{};
    /// Compartment OCID scoping every lookup. Required.
    std::string compartment_id;
    /// Logical cluster name, written to the `kythira-cluster` tag. Required.
    std::string cluster_name;
    /// OCID of a pre-existing, single-AD Instance Pool. Required.
    std::string instance_pool_id;
    /// TCP port the kythira process listens on; written into the returned address.
    std::uint16_t node_port{7000};
    /// Freshness window for the `kythira-last-heartbeat` tag. Zero disables the
    /// heartbeat rule entirely, leaving `lifecycleState` as the only signal.
    std::chrono::seconds heartbeat_timeout{30};
    /// How long after `timeCreated` an instance with no heartbeat tag yet still
    /// counts as live. Without this, every freshly launched instance would be
    /// classified unreachable and immediately replaced.
    std::chrono::seconds heartbeat_grace_period{120};
    /// Target node counts per Availability Domain.
    desired_topology<std::string> topology{};
    /// Maximum wait for a newly launched instance to appear untagged in the pool.
    std::chrono::seconds provision_timeout{300};
    /// Interval between `ListInstancePoolInstances` polls during provisioning.
    std::chrono::milliseconds poll_interval{5000};
    /// Extra freeform tags merged onto every managed instance. Cannot override
    /// the four well-known keys (Requirement 4.3).
    std::map<std::string, std::string> extra_tags{};
};

// ============================================================================
// oci_instance_pool_quorum_manager
// ============================================================================

/// @brief `quorum_manager` driving an OCI Instance Pool's size.
///
/// @tparam NodeId  Node identifier type; defaults to `std::uint64_t`.
/// @tparam Address Network address type; defaults to `std::string`.
template<typename NodeId = std::uint64_t, typename Address = std::string>
requires node_id<NodeId>
class oci_instance_pool_quorum_manager {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using placement_group_id_type = std::string;  ///< An Availability Domain.

    /// @brief Validates the config and reads the pool's placement configurations.
    ///
    /// The `GetInstancePool` call is not a formality: `provision_node` rejects a
    /// `target_group` the pool has no placement configuration for, and that check
    /// needs the pool's own answer rather than the caller's belief about it.
    /// Doing it once here also fails a misconfigured deployment at construction
    /// rather than at the first remediation, which is the worst possible moment.
    ///
    /// @throws std::invalid_argument on a missing required field (Requirement 2.2).
    /// @throws std::runtime_error when `GetInstancePool` fails (Requirement 2.3).
    explicit oci_instance_pool_quorum_manager(oci_instance_pool_quorum_manager_config cfg)
        : _cfg(std::move(cfg)), _http(_cfg.oci) {
        const auto require = [](const std::string& value, const char* field) {
            if (value.empty()) {
                throw std::invalid_argument(std::string("oci_instance_pool_quorum_manager: ") +
                                            field + " must be non-empty");
            }
        };
        require(_cfg.compartment_id, "compartment_id");
        require(_cfg.cluster_name, "cluster_name");
        require(_cfg.instance_pool_id, "instance_pool_id");
        if (_cfg.node_port == 0) {
            throw std::invalid_argument(
                "oci_instance_pool_quorum_manager: node_port must be non-zero");
        }
        if (!_cfg.oci.use_instance_principal) {
            require(_cfg.oci.tenancy_id, "oci.tenancy_id");
            require(_cfg.oci.user_id, "oci.user_id");
            require(_cfg.oci.fingerprint, "oci.fingerprint");
            require(_cfg.oci.private_key_pem, "oci.private_key_pem");
        }

        boost::json::value pool;
        try {
            pool = _http.request("iaas", "GET", "/20160918/instancePools/" + _cfg.instance_pool_id);
        } catch (const std::exception& ex) {
            throw std::runtime_error(
                std::string("oci_instance_pool_quorum_manager: GetInstancePool failed for ") +
                _cfg.instance_pool_id + ": " + ex.what());
        }
        const auto* obj = pool.if_object();
        if (obj == nullptr) {
            throw std::runtime_error(
                "oci_instance_pool_quorum_manager: GetInstancePool returned no object for " +
                _cfg.instance_pool_id);
        }
        if (const auto* placements = obj->if_contains("placementConfigurations");
            placements != nullptr) {
            if (const auto* arr = placements->if_array(); arr != nullptr) {
                for (const auto& entry : *arr) {
                    auto ad = oci_detail::json_string(entry, "availabilityDomain");
                    if (!ad.empty()) {
                        _availability_domains.push_back(std::move(ad));
                    }
                }
            }
        }
        if (_availability_domains.empty()) {
            throw std::runtime_error(
                "oci_instance_pool_quorum_manager: instance pool " + _cfg.instance_pool_id +
                " reported no placementConfigurations with an availabilityDomain");
        }
    }

    oci_instance_pool_quorum_manager(const oci_instance_pool_quorum_manager&) = delete;
    auto operator=(const oci_instance_pool_quorum_manager&)
        -> oci_instance_pool_quorum_manager& = delete;
    oci_instance_pool_quorum_manager(oci_instance_pool_quorum_manager&&) = default;
    auto operator=(oci_instance_pool_quorum_manager&&)
        -> oci_instance_pool_quorum_manager& = default;
    ~oci_instance_pool_quorum_manager() = default;

    /// @brief The Availability Domains this pool is configured to place into.
    ///
    /// Read once at construction. Exposed so a caller — or a test — can check the
    /// one-pool-per-AD deployment assumption without repeating the API call.
    [[nodiscard]] auto availability_domains() const -> const std::vector<std::string>& {
        return _availability_domains;
    }

    /// @brief Classifies every supplied node as live or unreachable
    ///        (Requirements 5.1-5.8).
    ///
    /// Liveness is `lifecycleState == RUNNING` **and** the heartbeat rule: a
    /// fresh `kythira-last-heartbeat` tag, or — when there is no such tag yet —
    /// an instance still inside `heartbeat_grace_period` of its `timeCreated`.
    /// `lifecycleState` alone would report a node whose kythira process has
    /// wedged as perfectly healthy, which is the failure this rule exists for.
    ///
    /// Makes no modification to any OCI resource (Requirement 5.7).
    auto assess_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/oci/instance_pool/list_instances",
                      throw std::runtime_error("fault: raft/oci/instance_pool/list_instances"););

            // Requirement 5.1: an empty cluster is healthy by definition and
            // costs no API call. Not just an optimisation — a caller polling an
            // empty cluster should not need OCI reachable at all.
            if (cluster.empty()) {
                return build_health(cluster, {});
            }

            const auto instances = describe_pool_instances();
            const auto now = std::time(nullptr);

            std::map<std::string, bool> live_map;
            for (const auto& inst : instances) {
                const auto id_tag = inst.freeform_tags.find(oci_detail::tag_node_id);
                if (id_tag == inst.freeform_tags.end()) {
                    continue;
                }
                live_map[id_tag->second] = is_live(inst, now);
            }
            return build_health(cluster, live_map);
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<
                quorum_health<NodeId, std::string>>(std::make_exception_ptr(std::runtime_error(
                std::string("oci_instance_pool_quorum_manager::assess_quorum: ") + ex.what())));
        }
    }

    /// @brief Assess, decommission what is unreachable, refill each AD's deficit
    ///        (Requirements 9.1-9.3).
    ///
    /// The same six steps as `aws_ec2_quorum_manager::maintain_quorum` and
    /// `aws_asg_quorum_manager::maintain_quorum`, and returning the
    /// **pre**-remediation health for the same reason they do: the caller asked
    /// what state the cluster was in, and answering with the state after this
    /// call's own repairs would hide the fault that triggered them.
    auto maintain_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/oci/instance_pool/maintain_quorum",
                      throw std::runtime_error("fault: raft/oci/instance_pool/maintain_quorum"););
        } catch (...) {
            return future_factory_default::makeExceptionalFuture<
                quorum_health<NodeId, std::string>>(std::current_exception());
        }

        quorum_health<NodeId, std::string> pre_health;
        try {
            pre_health = std::move(assess_quorum(cluster)).get();
        } catch (...) {
            // Requirement 9.3: no remediation is attempted on an assessment
            // failure. Provisioning against an unknown cluster state is how a
            // transient API error turns into a doubled cluster.
            return future_factory_default::makeExceptionalFuture<
                quorum_health<NodeId, std::string>>(std::current_exception());
        }

        std::map<std::string, NodeId> last_replaced;
        for (const auto& nid : pre_health.unreachable_nodes) {
            std::string group;
            for (const auto& np : cluster) {
                if (np.node_id == nid) {
                    group = np.group_id;
                    break;
                }
            }
            try {
                std::move(decommission_node(nid)).get();
                last_replaced[group] = nid;
            } catch (const std::exception& ex) {
                std::cerr << "[oci_instance_pool_quorum_manager::maintain_quorum] decommission of "
                          << node_id_str(nid) << " failed: " << ex.what() << "\n";
            }
        }

        for (const auto& gt : _cfg.topology.groups) {
            std::size_t live = 0;
            for (const auto& gh : pre_health.groups) {
                if (gh.group_id == gt.group_id) {
                    live = gh.live_count;
                    break;
                }
            }
            const auto deficit =
                static_cast<std::ptrdiff_t>(gt.target_count) - static_cast<std::ptrdiff_t>(live);
            for (std::ptrdiff_t i = 0; i < deficit; ++i) {
                std::optional<NodeId> hint;
                if (auto it = last_replaced.find(gt.group_id); it != last_replaced.end()) {
                    hint = it->second;
                }
                try {
                    std::move(provision_node(gt.group_id, hint)).get();
                } catch (const std::exception& ex) {
                    std::cerr << "[oci_instance_pool_quorum_manager::maintain_quorum] provision in "
                              << gt.group_id << " failed: " << ex.what() << "\n";
                }
            }
        }

        return future_factory_default::makeFuture(std::move(pre_health));
    }

    /// @brief Grows the pool by one, tags the new instance, and returns its
    ///        address (Requirements 6.1-6.9).
    ///
    /// The "newly launched instance" heuristic is *absence of a
    /// `kythira-node-id` tag*, the same one `aws_asg_quorum_manager` uses. It
    /// works because this manager is the only writer of that tag and writes it
    /// immediately; it does mean an operator who launches an instance into the
    /// pool by hand mid-provision can have it adopted instead.
    auto provision_node(std::string target_group, std::optional<NodeId> replacing)
        -> kythira::future_default<peer_info<NodeId, Address>> {
        try {
            fiu_do_on("raft/oci/instance_pool/update_size",
                      throw std::runtime_error("fault: raft/oci/instance_pool/update_size"););

            if (std::ranges::find(_availability_domains, target_group) ==
                _availability_domains.end()) {
                throw std::invalid_argument(
                    "oci_instance_pool_quorum_manager: instance pool " + _cfg.instance_pool_id +
                    " has no placement configuration for availability domain: " + target_group);
            }
            if (replacing.has_value()) {
                // Requirement 6.8: diagnostic only. The new instance gets a
                // fresh NodeId regardless — reusing the old one would resurrect
                // an identity the Raft log may still be reasoning about.
                std::cerr << "[oci_instance_pool_quorum_manager::provision_node] replacing node "
                          << node_id_str(*replacing) << " in " << target_group << "\n";
            }

            const auto pool =
                _http.request("iaas", "GET", "/20160918/instancePools/" + _cfg.instance_pool_id);
            const auto orig_size = oci_detail::json_int(pool, "size", -1);
            if (orig_size < 0) {
                throw std::runtime_error("GetInstancePool returned no usable size for " +
                                         _cfg.instance_pool_id);
            }

            set_pool_size(orig_size + 1);

            std::optional<oci_detail::instance_view> launched;
            std::vector<oci_detail::instance_view> snapshot;
            const auto deadline = std::chrono::steady_clock::now() + _cfg.provision_timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(_cfg.poll_interval);
                try {
                    snapshot = describe_pool_instances();
                } catch (const std::exception& ex) {
                    // A poll that failed is not a provisioning failure; the
                    // deadline is what bounds this loop.
                    std::cerr << "[oci_instance_pool_quorum_manager::provision_node] poll failed: "
                              << ex.what() << "\n";
                    continue;
                }
                for (const auto& inst : snapshot) {
                    if (!inst.freeform_tags.contains(oci_detail::tag_node_id)) {
                        launched = inst;
                        break;
                    }
                }
                if (launched.has_value()) {
                    break;
                }
            }

            if (!launched.has_value()) {
                // Requirement 6.7: best-effort rollback. Best-effort because the
                // instance may yet appear, and because there is nothing else
                // available — with no identified instance there is nothing
                // specific to terminate, the same limitation the ASG manager has.
                try {
                    set_pool_size(orig_size);
                } catch (const std::exception& ex) {
                    std::cerr << "[oci_instance_pool_quorum_manager::provision_node] size rollback "
                                 "to "
                              << orig_size << " failed: " << ex.what() << "\n";
                }
                throw std::runtime_error("timed out after " +
                                         std::to_string(_cfg.provision_timeout.count()) +
                                         "s waiting for a new instance in " + target_group);
            }

            // Computed from the same snapshot the candidate came from, rather
            // than a second listing: two scans could disagree, and the one that
            // matters is the one that identified the untagged instance.
            const NodeId new_id = next_node_id_from(snapshot);

            std::map<std::string, std::string> tags = _cfg.extra_tags;
            // Assigned after extra_tags so Requirement 4.3 holds: an operator
            // cannot redirect a managed instance by supplying its own
            // `kythira-node-id`.
            tags[oci_detail::tag_cluster] = _cfg.cluster_name;
            tags[oci_detail::tag_node_id] = node_id_str(new_id);
            tags[oci_detail::tag_group] = target_group;
            tags[oci_detail::tag_managed_by] = oci_detail::managed_by_value;
            apply_tags(launched->id, tags);

            const std::string private_ip = private_ip_of(launched->id);
            auto addr = static_cast<Address>(private_ip + ":" + std::to_string(_cfg.node_port));
            return future_factory_default::makeFuture(peer_info<NodeId, Address>{new_id, addr});
        } catch (const std::invalid_argument& ex) {
            // Requirement 6.1 names `std::invalid_argument` specifically for a
            // `target_group` this pool cannot place into, so it is rethrown as
            // itself rather than flattened into `std::runtime_error` with
            // everything else. The distinction is the caller's: a bad group is a
            // configuration error to fix, an OCI failure is one to retry.
            return future_factory_default::makeExceptionalFuture<peer_info<NodeId, Address>>(
                std::make_exception_ptr(std::invalid_argument(
                    std::string("oci_instance_pool_quorum_manager::provision_node: ") +
                    ex.what())));
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<peer_info<NodeId, Address>>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("oci_instance_pool_quorum_manager::provision_node: ") +
                    ex.what())));
        }
    }

    /// @brief Detaches and terminates the instance carrying @p node_id
    ///        (Requirements 7.1-7.8).
    ///
    /// Idempotent in both directions that matter: an unknown node and an already
    /// terminating one both resolve successfully without a detach call. A
    /// caller retrying after a partial failure must not be told the second
    /// attempt failed.
    auto decommission_node(const NodeId& node_id) -> kythira::future_default<void> {
        try {
            fiu_do_on("raft/oci/instance_pool/detach_instance",
                      throw std::runtime_error("fault: raft/oci/instance_pool/detach_instance"););

            const auto found = find_instance(node_id);
            if (!found.has_value()) {
                return future_factory_default::makeFuture();
            }
            if (found->lifecycle_state == "TERMINATING" || found->lifecycle_state == "TERMINATED") {
                return future_factory_default::makeFuture();
            }

            boost::json::object detach;
            detach["instanceId"] = found->id;
            detach["isDecrementSize"] = true;
            detach["isAutoTerminate"] = true;
            try {
                (void)_http.request(
                    "iaas", "POST",
                    "/20160918/instancePools/" + _cfg.instance_pool_id + "/actions/detachInstance",
                    boost::json::serialize(detach));
            } catch (const std::exception& ex) {
                // Requirement 7.5: only "not in this pool" is forgiven. Anything
                // else is a real failure and the caller has to see it — quietly
                // succeeding would leave a billed instance running with the
                // orchestrator believing it gone.
                const std::string what = ex.what();
                if (what.find("not found") == std::string::npos &&
                    what.find("NotFound") == std::string::npos) {
                    throw;
                }
                return future_factory_default::makeFuture();
            }

            // Requirement 7.6: a bounded consistency poll, so the next
            // assess_quorum observes the instance as gone rather than racing the
            // control plane and re-reporting it live.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
            for (;;) {
                // Checked before sleeping, not after: a detach that took effect
                // immediately is the common case, and sleeping first would put a
                // two-second floor under every decommission for nothing.
                try {
                    if (get_instance(found->id).lifecycle_state != "RUNNING") {
                        break;
                    }
                } catch (const std::exception&) {
                    // A terminated instance can stop being readable at all;
                    // that is the state this loop is waiting for.
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::seconds{2});
            }
            return future_factory_default::makeFuture();
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<void>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("oci_instance_pool_quorum_manager::decommission_node: ") +
                    ex.what())));
        }
    }

    /// @brief Returns the configured topology (Requirements 8.1-8.2). No API call.
    [[nodiscard]] auto topology() const -> kythira::desired_topology<std::string> {
        return _cfg.topology;
    }

    /// @brief The `NodeId` this manager would assign next.
    ///
    /// Max-of-parsed-tags plus one, scanned across the whole pool. Exposed for
    /// tests and diagnostics; `provision_node` uses the snapshot it already has
    /// rather than calling this, so the two cannot disagree.
    ///
    /// **NodeIds are reused after a decommission.** The scan can only see
    /// instances the pool still lists, and `decommission_node` detaches its
    /// target — so once the highest-numbered node is removed, the next
    /// provision gets that number back. Requirement 6.4 scopes the scan to
    /// `ListInstancePoolInstances`, and there is no OCI call that would show a
    /// detached instance's tags, so this is a property of the design rather
    /// than an oversight in it: a deployment that cannot tolerate a recycled
    /// identity has to keep the assignment somewhere outside the pool. It is
    /// pinned by a test rather than left to be rediscovered.
    [[nodiscard]] auto next_node_id() const -> NodeId {
        return next_node_id_from(describe_pool_instances());
    }

    /// @brief Merges @p new_tags onto the instance's current `freeformTags`
    ///        (Requirement 4.2, design.md Property 1).
    ///
    /// `UpdateInstance` **replaces** the tag map — it does not merge, unlike AWS
    /// `CreateTags`. Writing only the new keys therefore deletes every tag an
    /// operator or another system set, and `UpdateInstance` reports success
    /// either way. There is no error to notice, which is why this is a named
    /// helper with its own test rather than three lines inlined into
    /// `provision_node`.
    [[nodiscard]] auto merged_tags(const std::string& instance_id,
                                   const std::map<std::string, std::string>& new_tags) const
        -> std::map<std::string, std::string> {
        auto current = get_instance(instance_id).freeform_tags;
        for (const auto& [key, value] : new_tags) {
            current[key] = value;
        }
        return current;
    }

private:
    static auto node_id_str(const NodeId& id) -> std::string {
        if constexpr (std::is_same_v<NodeId, std::string>) {
            return id;
        } else {
            return std::to_string(id);
        }
    }

    /// Requirement 5.3's ladder, in one place so `assess_quorum` reads as
    /// classification rather than as policy.
    [[nodiscard]] auto is_live(const oci_detail::instance_view& inst, std::time_t now) const
        -> bool {
        if (inst.lifecycle_state != "RUNNING") {
            return false;
        }
        if (_cfg.heartbeat_timeout.count() == 0) {
            return true;
        }
        if (const auto beat = inst.freeform_tags.find(oci_detail::tag_last_heartbeat);
            beat != inst.freeform_tags.end()) {
            try {
                const auto stamp = static_cast<std::time_t>(std::stoll(beat->second));
                return now - stamp <= _cfg.heartbeat_timeout.count();
            } catch (const std::exception&) {
                // A tag this manager cannot read is not evidence of health.
                return false;
            }
        }
        // No heartbeat yet: live only while still inside the launch grace
        // period. An unparseable `timeCreated` gives no age to compare against,
        // so it falls through to unreachable rather than being assumed young.
        const auto created = oci_detail::parse_rfc3339_utc(inst.time_created);
        if (!created.has_value()) {
            return false;
        }
        return now - *created <= _cfg.heartbeat_grace_period.count();
    }

    [[nodiscard]] auto list_pool_instance_ids() const -> std::vector<std::string> {
        const auto listed = _http.request("iaas", "GET",
                                          "/20160918/instancePools/" + _cfg.instance_pool_id +
                                              "/instances?compartmentId=" + _cfg.compartment_id);
        std::vector<std::string> ids;
        const auto* arr = listed.if_array();
        if (arr == nullptr) {
            return ids;
        }
        ids.reserve(arr->size());
        for (const auto& entry : *arr) {
            auto id = oci_detail::json_string(entry, "id");
            if (!id.empty()) {
                ids.push_back(std::move(id));
            }
        }
        return ids;
    }

    [[nodiscard]] auto get_instance(const std::string& instance_id) const
        -> oci_detail::instance_view {
        const auto body = _http.request("iaas", "GET", "/20160918/instances/" + instance_id);
        return oci_detail::instance_view{
            .id = instance_id,
            .lifecycle_state = oci_detail::json_string(body, "lifecycleState"),
            .availability_domain = oci_detail::json_string(body, "availabilityDomain"),
            .time_created = oci_detail::json_string(body, "timeCreated"),
            .freeform_tags = oci_detail::freeform_tags_of(body),
        };
    }

    /// `ListInstancePoolInstances` plus a `GetInstance` per member.
    ///
    /// The second half is why this is concurrent (Requirement 5.2): OCI has no
    /// batched multi-OCID read equivalent to EC2's `DescribeInstances`, so a
    /// serial loop would make assessment latency scale with cluster size where
    /// AWS's is one call. Bounded at eight because the point is to hide latency,
    /// not to hand a control plane a thread per node and collect 429s.
    [[nodiscard]] auto describe_pool_instances() const -> std::vector<oci_detail::instance_view> {
        const auto ids = list_pool_instance_ids();
        std::vector<oci_detail::instance_view> views(ids.size());
        oci_detail::parallel_for(ids.size(), max_describe_concurrency,
                                 [&](std::size_t i) { views[i] = get_instance(ids[i]); });
        return views;
    }

    [[nodiscard]] auto next_node_id_from(
        const std::vector<oci_detail::instance_view>& instances) const -> NodeId {
        std::uint64_t highest = 0;
        for (const auto& inst : instances) {
            const auto tag = inst.freeform_tags.find(oci_detail::tag_node_id);
            if (tag == inst.freeform_tags.end()) {
                continue;
            }
            try {
                highest = std::max(highest, static_cast<std::uint64_t>(std::stoull(tag->second)));
            } catch (const std::exception&) {
                // A tag nobody here wrote. Ignoring it is right: it cannot
                // collide with an id this manager assigns, because this manager
                // only ever assigns parseable ones.
            }
        }
        return static_cast<NodeId>(highest + 1);
    }

    [[nodiscard]] auto find_instance(const NodeId& node) const
        -> std::optional<oci_detail::instance_view> {
        const auto wanted = node_id_str(node);
        for (auto& inst : describe_pool_instances()) {
            const auto tag = inst.freeform_tags.find(oci_detail::tag_node_id);
            if (tag != inst.freeform_tags.end() && tag->second == wanted) {
                return inst;
            }
        }
        return std::nullopt;
    }

    auto apply_tags(const std::string& instance_id,
                    const std::map<std::string, std::string>& tags) const -> void {
        boost::json::object merged;
        for (const auto& [key, value] : merged_tags(instance_id, tags)) {
            merged[key] = value;
        }
        boost::json::object update;
        update["freeformTags"] = std::move(merged);
        (void)_http.request("iaas", "PUT", "/20160918/instances/" + instance_id,
                            boost::json::serialize(update));
    }

    [[nodiscard]] auto private_ip_of(const std::string& instance_id) const -> std::string {
        const auto attachments =
            _http.request("iaas", "GET",
                          "/20160918/vnicAttachments?compartmentId=" + _cfg.compartment_id +
                              "&instanceId=" + instance_id);
        const auto* arr = attachments.if_array();
        if (arr == nullptr || arr->empty()) {
            throw std::runtime_error("no VNIC attachments for instance " + instance_id);
        }
        for (const auto& attachment : *arr) {
            const auto vnic_id = oci_detail::json_string(attachment, "vnicId");
            if (vnic_id.empty()) {
                continue;
            }
            const auto vnic = _http.request("iaas", "GET", "/20160918/vnics/" + vnic_id);
            auto ip = oci_detail::json_string(vnic, "privateIp");
            if (!ip.empty()) {
                return ip;
            }
        }
        throw std::runtime_error("no VNIC of instance " + instance_id + " reported a privateIp");
    }

    auto set_pool_size(std::int64_t size) const -> void {
        boost::json::object update;
        update["size"] = size;
        (void)_http.request("iaas", "PUT", "/20160918/instancePools/" + _cfg.instance_pool_id,
                            boost::json::serialize(update));
    }

    /// Identical to `aws_asg_quorum_manager::build_health`, deliberately: the
    /// per-group breakdown is a property of the topology and the supplied
    /// cluster, not of the cloud, and Requirement 5.5 asks for exactly the same
    /// mapping. Divergence here would be a difference in behaviour with no
    /// difference in cause.
    [[nodiscard]] auto build_health(const std::vector<node_placement<NodeId, std::string>>& cluster,
                                    const std::map<std::string, bool>& live_map) const
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        std::vector<NodeId> unreachable;
        std::size_t live_count = 0;
        std::map<std::string, std::size_t> group_live;
        for (const auto& np : cluster) {
            const auto it = live_map.find(node_id_str(np.node_id));
            if (it != live_map.end() && it->second) {
                ++live_count;
                group_live[np.group_id]++;
            } else {
                unreachable.push_back(np.node_id);
            }
        }

        std::vector<placement_group_health<NodeId, std::string>> groups;
        for (const auto& gt : _cfg.topology.groups) {
            std::size_t group_live_count = 0;
            if (const auto it = group_live.find(gt.group_id); it != group_live.end()) {
                group_live_count = it->second;
            }
            std::vector<NodeId> group_unreachable;
            for (const auto& nid : unreachable) {
                for (const auto& np : cluster) {
                    if (np.node_id == nid && np.group_id == gt.group_id) {
                        group_unreachable.push_back(nid);
                    }
                }
            }
            groups.push_back({.group_id = gt.group_id,
                              .live_count = group_live_count,
                              .target_count = gt.target_count,
                              .unreachable_nodes = std::move(group_unreachable)});
        }

        return future_factory_default::makeFuture(quorum_health<NodeId, std::string>{
            .status = compute_quorum_status(live_count, cluster.size()),
            .live_node_count = live_count,
            .total_node_count = cluster.size(),
            .unreachable_nodes = std::move(unreachable),
            .groups = std::move(groups),
        });
    }

    /// The same four-level mapping every quorum manager in this tree uses.
    static auto compute_quorum_status(std::size_t live, std::size_t total) -> quorum_status {
        if (total == 0) {
            return quorum_status::healthy;
        }
        const std::size_t majority = total / 2 + 1;
        if (live < majority) {
            return quorum_status::lost;
        }
        if (live == majority) {
            return quorum_status::critical;
        }
        if (live < total) {
            return quorum_status::degraded;
        }
        return quorum_status::healthy;
    }

    static constexpr std::size_t max_describe_concurrency = 8;

    oci_instance_pool_quorum_manager_config _cfg;
    oci_http_client _http;
    std::vector<std::string> _availability_domains;
};

static_assert(quorum_manager<oci_instance_pool_quorum_manager<std::uint64_t, std::string>,
                             std::uint64_t, std::string, std::string>,
              "oci_instance_pool_quorum_manager must satisfy quorum_manager");

}  // namespace kythira
