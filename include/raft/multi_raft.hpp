// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file multi_raft.hpp
/// @brief `multi_raft<Types, Key, GroupId>` — the host that owns many
///        `node<Types>` instances, demultiplexes one transport across them,
///        and drives them all from one batched tick.
///
/// Declarations only; definitions are in `multi_raft_impl.hpp`, following
/// `raft.hpp`'s own split. Include `multi_raft_impl.hpp` to use the class.
///
/// See `.kiro/specs/multi-raft/` design §4.
///
/// ### Why this is a layer above `node<Types>` and not a rewrite of it
///
/// Two properties of the existing code make the layering work with almost no
/// surgery on the consensus core:
///
/// 1. **`node<Types>` is externally ticked.** It has no timer thread of its
///    own — `check_election_timeout()`, `check_heartbeat_timeout()` and
///    `replicate_to_followers()` are called from outside (see
///    `cmd/chaos_node/main.cpp:125`). A multi-group host is therefore just a
///    better caller, which is exactly the shape TiKV's batched ready loop
///    wants.
/// 2. **Every dependency is a concept.** A *group-scoped view* of a transport
///    satisfies `network_client` / `network_server` (see
///    `group_transport.hpp`), so the consensus core cannot tell it is not
///    alone in the process.

#include <raft/group_storage.hpp>
#include <raft/group_transport.hpp>
#include <raft/shard_commands.hpp>
#include <raft/split_merge_policy.hpp>
#include <raft/splittable_state_machine.hpp>
#include <raft/raft.hpp>
#include <raft/shard_exceptions.hpp>
#include <raft/shard_map.hpp>
#include <raft/latency_digest.hpp>
#include <raft/load_split_sampler.hpp>
#include <raft/shard_placement_driver.hpp>
#include <raft/shard_types.hpp>
#include <raft/striped_executor.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace kythira {

// ─────────────────────────────────────────────────────────────────────────────
// Per-group type bundle
// ─────────────────────────────────────────────────────────────────────────────

/// @brief `Types`, with the transport slots replaced by group-scoped views.
///
/// Inheriting and shadowing rather than restating the bundle: a `Types` bundle
/// carries a dozen aliases and a future one may carry more, and a hand-copied
/// list would silently drop whichever alias was added last.
///
/// @tparam Types   The host's own type bundle.
/// @tparam GroupId Must satisfy `raft_group_id`.
/// @tparam Demux   The `multi_group_network_server` instantiation this host owns.
template<typename Types, typename GroupId, typename Demux> struct group_scoped_types : Types {
    using network_client_type = group_scoped_client<
        typename Types::network_client_type, GroupId,
        group_rpc_messages<typename Types::node_id_type, typename Types::term_id_type,
                           typename Types::log_index_type, typename Types::log_entry_type,
                           GroupId>>;
    using network_server_type = group_scoped_server<Demux>;
};

// ─────────────────────────────────────────────────────────────────────────────
// Tick and hibernation
// ─────────────────────────────────────────────────────────────────────────────

/// @brief What one `tick()` did, for Requirement 5.7.
///
/// Per-phase durations rather than one total: the four phases fail in
/// different ways — a slow persist phase is a storage problem, a slow send
/// phase is a network problem — and one number cannot distinguish them.
struct tick_report {
    std::size_t _ready_count{0};        ///< Groups driven this tick.
    std::size_t _hibernating_count{0};  ///< Groups skipped because they are hibernating.
    std::size_t _total_count{0};        ///< Groups in the registry.
    std::size_t _batch_size{0};         ///< Groups covered by one durability barrier.

    std::chrono::nanoseconds _persist_duration{};
    std::chrono::nanoseconds _send_duration{};
    std::chrono::nanoseconds _apply_duration{};
    std::chrono::nanoseconds _policy_duration{};

    /// @brief Whether the policy phase ran this tick (it runs on its own interval).
    bool _policy_ran{false};

    [[nodiscard]] auto total_duration() const -> std::chrono::nanoseconds {
        return _persist_duration + _send_duration + _apply_duration + _policy_duration;
    }
};

/// @brief When groups are allowed to hibernate.
enum class hibernation_mode : std::uint8_t {
    /// @brief Never. Every group is ticked every tick.
    off = 0,
    /// @brief Always, once a group qualifies.
    on = 1,
    /// @brief Only once the registry exceeds `_hibernation_group_threshold`.
    ///
    /// The default. Below the threshold the heartbeat traffic hibernation
    /// saves is not worth its cost in tooling confusion — TiKV RFC 0082 is
    /// candid that hibernation "is not always working as expected" under
    /// random access, so it is a knob here rather than an always-on behaviour.
    auto_above_group_count = 2,
};

/// @brief One durability barrier spanning every ready group's persist phase.
///
/// Supplied by a caller whose engine spans groups (one store with a
/// group-prefixed key space, say). Without it, `tick()` falls back to
/// per-group batching through `batched_persistence_engine`, which still
/// collapses each group's N appends into one barrier but pays one barrier per
/// ready group rather than one per tick.
///
/// This is the honest shape of the constraint: a single barrier for N groups
/// requires a store that spans N groups, and no wrapper can manufacture one
/// from N independent engines.
struct tick_batch_controller {
    std::function<void()> _begin;
    std::function<void()> _commit;
    std::function<void()> _abort;

    [[nodiscard]] auto valid() const -> bool {
        return static_cast<bool>(_begin) && static_cast<bool>(_commit) && static_cast<bool>(_abort);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Partitioning
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Extracts the routing key from a serialised command.
///
/// One function, deliberately. Kythira never parses a command — the
/// application already has a parser and hands over just the routing key. For a
/// state machine whose commands carry several keys, `key_of` returns the
/// *routing* key and the state machine owns the rest, with `can_split_at`
/// (design §6.4) as the tool that keeps a multi-key command's keys inside one
/// shard.
template<typename P, typename Key>
concept partitioner = requires(const P& p, const std::vector<std::byte>& command) {
    { p.key_of(command) } -> std::same_as<Key>;
};

/// @brief Type-erases a `partitioner` for storage in `multi_raft_config`.
///
/// The concept is the contract an application implements; this is how the host
/// holds one without becoming a template over it. Keeping both means an
/// application's partitioner is concept-checked at the call site rather than
/// silently accepted as any old callable.
template<typename Key, partitioner<Key> P>
[[nodiscard]] auto make_partitioner(P p) -> std::function<Key(const std::vector<std::byte>&)> {
    return [p = std::move(p)](const std::vector<std::byte>& command) { return p.key_of(command); };
}

// ─────────────────────────────────────────────────────────────────────────────
// The signal surface (design §6.2, §6.6)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Which channel asked for an operation.
///
/// Precedence is `admin` ▸ `placement_driver` ▸ `policy`, and a loser is logged
/// with `preempted_by` rather than silently dropped — an operator debugging
/// "why didn't my policy fire" needs to see that the placement driver outranked
/// it (Requirement 15.6).
enum class signal_channel : std::uint8_t {
    admin = 0,
    placement_driver = 1,
    policy = 2,
};

inline auto to_string(signal_channel c) -> std::string {
    switch (c) {
        case signal_channel::admin:
            return "admin";
        case signal_channel::placement_driver:
            return "placement_driver";
        case signal_channel::policy:
            return "policy";
        default:
            return "unknown";
    }
}

/// @brief Why the arbiter refused an operation.
///
/// A named gate rather than a bool, because thresholds are untunable without
/// it: an operator who cannot see `split.rejected{gate=cooldown}` will conclude
/// the feature is broken.
enum class arbiter_gate : std::uint8_t {
    admitted = 0,
    state = 1,               ///< The shard is not `stable` (or `frozen` and this is not admin).
    cooldown = 2,            ///< Inside `split_merge_interval` of the last operation.
    concurrency_limit = 3,   ///< `max_concurrent_split_merge` already in flight.
    globally_disabled = 4,   ///< The kill switch is off.
    epoch = 5,               ///< The epoch moved since the decision was computed.
    no_valid_split_key = 6,  ///< Every candidate was vetoed and none was suggested.
    alignment_required = 7,  ///< A merge whose replica sets are not colocated.
    pd_unavailable = 8,      ///< The shard-id authority could not be reached.
    not_leader = 9,          ///< This replica does not lead the shard.
    preempted = 10,          ///< A higher-precedence channel holds the shard.
    /// @brief The proposal named more keys than `batch_split_limit` allows.
    ///
    /// The only gate that **truncates rather than refuses**. A composition
    /// (design §6.1.3) unions its members' split keys, so it can exceed a limit
    /// every member respected individually — and the right answer there is
    /// fewer children, not no split. It is still counted and logged, because
    /// truncation overrides a decision a policy actually made, and a silent
    /// override is the kind of thing that is discovered during an incident.
    split_keys_truncated = 11,
};

inline auto to_string(arbiter_gate g) -> std::string {
    switch (g) {
        case arbiter_gate::admitted:
            return "admitted";
        case arbiter_gate::state:
            return "state";
        case arbiter_gate::cooldown:
            return "cooldown";
        case arbiter_gate::concurrency_limit:
            return "concurrency_limit";
        case arbiter_gate::globally_disabled:
            return "globally_disabled";
        case arbiter_gate::epoch:
            return "epoch";
        case arbiter_gate::no_valid_split_key:
            return "no_valid_split_key";
        case arbiter_gate::alignment_required:
            return "alignment_required";
        case arbiter_gate::pd_unavailable:
            return "pd_unavailable";
        case arbiter_gate::not_leader:
            return "not_leader";
        case arbiter_gate::preempted:
            return "preempted_by";
        case arbiter_gate::split_keys_truncated:
            return "split_keys_truncated";
        default:
            return "unknown";
    }
}

/// @brief Operator control over one split (design §6.2).
struct split_options {
    /// @brief Resolve on apply, not merely on commit.
    ///
    /// Default true. Resolving on commit would return success before the
    /// children exist, and an operator scripting a pre-split followed by a bulk
    /// load would race their own split. Commit is not the interesting event
    /// here; apply is.
    bool _wait_for_apply{true};

    /// Ask the placement driver to spread the new leaders. Not optional for a
    /// load split: children whose leaders both land on the machine that was
    /// already hot have accomplished nothing.
    bool _scatter_children{false};

    /// @brief Skip `split_merge_interval`. Off, and it exists.
    ///
    /// The cooldown guards against *automatic* oscillation. An operator who has
    /// diagnosed a hot shard should not have to wait an hour or edit config —
    /// but they should have to say so.
    bool _override_cooldown{false};

    /// @brief Fall back to the state machine's suggestion when a named key is
    /// vetoed, rather than failing.
    ///
    /// On by default: an operator naming a forbidden key gets a nearby valid
    /// one and a log line, unless they explicitly ask for the strict behaviour.
    bool _allow_state_machine_veto{true};

    signal_channel _channel{signal_channel::admin};
    split_reason _reason{split_reason::admin};

    /// @brief Which policy decided, for the `policy` metric dimension.
    ///
    /// `reason` alone stops identifying the decider once policies compose:
    /// two members can both return `size`. Borrowed pointer with the same
    /// lifetime rule as `split_decision::_policy` — static storage.
    const char* _policy{nullptr};
};

/// @brief Operator control over one merge (design §6.2).
struct merge_options {
    bool _wait_for_apply{true};

    /// @brief Let the placement driver colocate the replicas first, then retry.
    ///
    /// **Off by default.** Colocating replicas moves data. An operator asking
    /// for a merge should not silently trigger replica movement across the
    /// cluster; they should be told alignment is needed and opt in.
    bool _auto_align{false};
    std::chrono::milliseconds _align_timeout{std::chrono::minutes{5}};

    signal_channel _channel{signal_channel::admin};
    merge_reason _reason{merge_reason::admin};

    /// @brief Which policy decided. See `split_options::_policy`.
    const char* _policy{nullptr};
};

/// @brief What the arbiter decided, and why.
template<raft_group_id GroupId> struct arbiter_decision {
    bool _admitted{false};
    arbiter_gate _gate{arbiter_gate::admitted};
    signal_channel _channel{signal_channel::policy};
    /// Set when `_gate == preempted`: the channel that holds the shard.
    std::optional<signal_channel> _preempted_by{};

    [[nodiscard]] explicit operator bool() const { return _admitted; }
};

/// @brief A role or membership change on one group, for `set_report_listener`.
///
/// Mirrors MicroRaft's `RaftNodeReportListener`: an operator watching a
/// thousand groups needs the transitions pushed at them, not polled for.
template<raft_group_id GroupId, typename NodeId> struct group_report {
    GroupId _group_id{};
    server_state _role{server_state::follower};
    std::uint64_t _term{0};
    std::uint64_t _commit_index{0};
    std::vector<NodeId> _voters{};
    shard_operation_state _operation{shard_operation_state::stable};
};

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Named-parameter aggregate for `multi_raft`, mirroring `node_config`.
///
/// The per-group components arrive as **factories** rather than as values,
/// because a group's store is scoped by its construction argument (a data
/// directory, an object-key prefix) and there is no way to derive one engine
/// per group from a single prototype. The same shape is used for the state
/// machine, the membership manager and the logger so that a caller can give
/// each group its own configured instance where it matters.
template<raft_types Types, shard_key Key, raft_group_id GroupId = std::uint64_t>
struct multi_raft_config {
    using node_id_type = typename Types::node_id_type;
    using descriptor_type = shard_descriptor<GroupId, Key, node_id_type>;

    // ── required ─────────────────────────────────────────────────────────────

    /// This node's identifier, shared by every group's replica.
    node_id_type node_id{};
    /// The one shared client. Its connection pool is reused across every group.
    typename Types::network_client_type network_client;
    /// The one shared server, moved into the demultiplexer.
    typename Types::network_server_type network_server;
    /// Builds a store for one group, scoped so no two groups collide. See
    /// `group_data_dir()` / `group_key_prefix()`.
    std::function<typename Types::persistence_engine_type(const GroupId&)> store_factory;
    /// The host's own logger, for host-level messages only.
    ///
    /// Groups do **not** share it: each gets one from `logger_factory`, or a
    /// default-constructed one. `console_logger` is move-only and several
    /// metrics backends own a connection two groups must not share, so copying
    /// a prototype per group is not available even where it would be wanted.
    typename Types::logger_type logger{};
    /// The host's own metrics sink; see `logger`.
    typename Types::metrics_type metrics{};

    // ── optional ─────────────────────────────────────────────────────────────

    std::function<typename Types::state_machine_type(const GroupId&)> state_machine_factory{};
    std::function<typename Types::membership_manager_type(const GroupId&)> membership_factory{};
    std::function<typename Types::logger_type(const GroupId&)> logger_factory{};
    std::function<typename Types::metrics_type(const GroupId&)> metrics_factory{};

    /// Raft timing and policy, copied into every group.
    typename Types::configuration_type config{};

    /// Serial-executor stripes. `0` means `striped_serial_executor::default_stripe_count()`.
    /// Never one thread per group — that is the implementation that stops
    /// working at a few hundred groups.
    std::size_t executor_stripes{0};

    hibernation_mode hibernation{hibernation_mode::auto_above_group_count};
    std::size_t hibernation_group_threshold{64};
    /// Idle time before a group may hibernate. Defaults to
    /// `10 * config.heartbeat_interval()`.
    std::optional<std::chrono::milliseconds> hibernate_after{};

    /// How often the policy phase runs. Policy evaluation is far coarser than
    /// the tick and running it every tick would dominate the loop.
    std::chrono::milliseconds policy_interval{std::chrono::seconds{10}};

    /// @brief Channel (c′): the load-based split sampler (design §6.3).
    ///
    /// Off by default. When off, its cost is one predictably-taken branch per
    /// routed request (Requirement 9.7) — `observe()` returns on a state check
    /// before touching anything else, and the host never takes the sampler
    /// lock at all.
    load_split_sampler_config load_split{};

    /// @brief Sub-windows in each group's latency digest (design §6.1.4).
    ///
    /// The digests rotate on the policy tick, so the percentiles a policy reads
    /// cover the last `latency_window_count * policy_interval` and nothing
    /// older. Higher is smoother and slower to react; lower is twitchier and
    /// decays faster. One degenerates to "everything since the last policy
    /// tick", which is legitimate but coarse.
    std::size_t latency_window_count{latency_digest::k_default_windows};

    /// One barrier per tick instead of one per ready group; see the type's docs.
    std::optional<tick_batch_controller> batch_controller{};

    /// Where the host's own durable state lives (shard map, tombstones).
    /// Empty means the host keeps them in memory only.
    std::filesystem::path host_data_dir{};

    /// How long a tombstone is kept before garbage collection.
    std::chrono::milliseconds tombstone_horizon{std::chrono::hours{24}};

    /// Extracts a command's routing key. Optional: a caller that always names
    /// the key explicitly needs none, but without it the cross-shard admission
    /// check cannot run and a mis-routed command would be applied silently.
    std::function<Key(const std::vector<std::byte>&)> partitioner{};

    /// How many times a routed request re-resolves before giving up.
    ///
    /// Each retry follows a *specific* repair — a refreshed routing row, a
    /// finished merge — so the bound is small on purpose. Exceeding it
    /// surfaces the last real error rather than a generic timeout, because
    /// "epoch mismatch five times" and "timed out" call for different actions.
    std::size_t max_route_retries{5};

    /// Backoff between routing retries.
    std::chrono::milliseconds route_retry_backoff{std::chrono::milliseconds{5}};

    // ── split (design §5.3) ──────────────────────────────────────────────────

    /// @brief Allocates `n` cluster-unique group ids for a split's children.
    ///
    /// Returning fewer than `n` (including none) means the authority is
    /// unavailable, and the split is **abandoned** rather than queued. This is
    /// not optional and not local: every replica must derive identical group
    /// ids, and only a cluster-scope authority can guarantee uniqueness.
    /// Inventing ids locally is how two different shards in two partitions end
    /// up with the same group id — TiKV makes the same call with
    /// `AskBatchSplit`. Phase 10's placement driver supplies this; until then a
    /// static allocator serves a pre-split deployment.
    std::function<std::vector<GroupId>(std::size_t)> allocate_group_ids{};

    /// @brief Turns a routing key into the bytes that ride inside a split entry.
    ///
    /// Every replica must decode exactly the keys the leader encoded, or they
    /// cut their state machines in different places.
    std::function<std::vector<std::byte>(const Key&)> encode_key{};
    std::function<Key(const std::vector<std::byte>&)> decode_key{};

    /// @brief Maximum children one split may produce (TiKV RFC 0006's batch split).
    ///
    /// A one-key-at-a-time API cannot express the fix RFC 0006 exists for: "the
    /// split speed cannot keep up with write speed" under a fast sequential
    /// write.
    std::size_t batch_split_limit{10};

    /// @brief Whether the RIGHTMOST child inherits the parent's group id, log
    /// and term. Default is the leftmost, matching the `(-inf, k)` reading.
    bool right_derive{false};

    // ── lazy replica creation (Requirement 12, design §5.4 step F) ───────────

    /// @brief Whether a message for an unknown group may create a replica.
    ///
    /// On by default: it is how a node held offline through a split acquires
    /// its child, and the alternative is a control-plane push that has to
    /// retry until the node comes back.
    bool lazy_replica_creation{true};

    /// @brief Looks a group's descriptor up outside the local routing map.
    ///
    /// Consulted only when the local map has no row, and only once per group
    /// per `unknown_group_lookup_interval` — an unknown group id arriving at
    /// message rate must not turn into a control-plane query at message rate.
    /// Phase 10's placement driver supplies this.
    std::function<std::optional<descriptor_type>(const GroupId&)> lookup_descriptor{};

    /// @brief Minimum gap between external lookups for the same group id.
    std::chrono::milliseconds unknown_group_lookup_interval{std::chrono::seconds{1}};

    /// @brief Extra election-timeout stagger applied to each non-derived child.
    ///
    /// N children electing simultaneously across the same machines produce a
    /// burst of RequestVote traffic and a correlated latency spike on every
    /// child. Staggering keeps the unavailability window to roughly one
    /// election timeout for one child, and near zero for the derived one, which
    /// campaigns immediately if it sits on the parent's leader.
    std::chrono::milliseconds child_election_stagger{std::chrono::milliseconds{50}};

    // ── merge (Requirement 13, design §5.5) ──────────────────────────────────

    /// @brief Enable the timing-based rollback variant.
    ///
    /// **Off by default, and its assumption is stated first: it requires
    /// bounded clock skew, which nothing else in Kythira assumes.** With it
    /// off, a source frozen by `merge_prepare` is released only by observing a
    /// committed `merge_abandoned` in the target's own log. With it on, the
    /// source releases itself after a deadline — which is faster, and which
    /// makes "two shards own one range" reachable if the clocks disagree.
    ///
    /// An operator who turns this on is taking on an assumption the rest of the
    /// system does not make.
    bool merge_lease_mode{false};

    // ── the arbiter (Requirement 15, design §6.6) ────────────────────────────

    /// @brief The declarative policy, channel (a). Absent means no automatic
    /// split or merge — which is a legitimate, fully static deployment.
    std::function<split_decision<Key>(const shard_stats<GroupId, Key, node_id_type>&)>
        evaluate_split{};
    std::function<merge_decision(const shard_stats<GroupId, Key, node_id_type>&,
                                 const shard_stats<GroupId, Key, node_id_type>&)>
        evaluate_merge{};

    /// @brief Global kill switch for the AUTOMATIC channels.
    ///
    /// Admin commands are unaffected. Turning this off is what an operator
    /// reaches for when the cluster is misbehaving and they want it to stop
    /// moving while they look.
    bool automatic_split_merge_enabled{true};

    /// @brief Minimum time between operations on one shard, enforced by the
    /// HOST rather than by the policy (Requirement 7.6).
    ///
    /// A custom policy that forgets its own cooldown still cannot oscillate.
    /// Defence in depth on the one knob whose misconfiguration is unbounded.
    std::chrono::milliseconds split_merge_interval{std::chrono::hours{1}};

    /// @brief How many split or merge operations may be in flight at once.
    ///
    /// Enforced BEFORE proposing, never by aborting something already
    /// committed — the same discipline as TiKV's schedule limits, and for the
    /// same reason: a rebalancing storm that saturates the network is worse
    /// than a slow rebalance.
    std::size_t max_concurrent_split_merge{4};

    /// @brief How long a frozen source waits before reporting itself stalled.
    ///
    /// A stuck merge leaves the source unavailable but *correct*. That is the
    /// right trade, and it is surfaced rather than left to be guessed at.
    std::chrono::milliseconds merge_stall_warning_after{std::chrono::seconds{30}};

    // ── the placement driver, channel (d) (Requirement 14, design §7) ────────

    using shard_report_type = shard_report<GroupId, Key, node_id_type>;
    using shard_operation_type = shard_operation<GroupId, Key, node_id_type>;
    using node_report_type = node_report<node_id_type>;

    /// @brief One call per interval carrying EVERY local leader's report.
    ///
    /// Batched, not per shard. At a thousand shards and a ten-second interval
    /// the difference is 100 RPS of control-plane traffic from one machine
    /// against 0.1 RPS, and a control plane whose load grows with shard count
    /// fails at exactly the scale sharding was adopted to reach.
    ///
    /// Called from `tick()`, on the ticking thread, once per
    /// `heartbeat_interval`. A driver that cannot answer promptly should return
    /// an empty operator list rather than block: operators are advisory and
    /// there is another heartbeat coming.
    std::function<std::vector<shard_operation_type>(const std::vector<shard_report_type>&)>
        report_shard_heartbeat{};

    /// @brief One call per interval describing this machine.
    std::function<void(const node_report_type&)> report_node_heartbeat{};

    /// @brief Told at apply time rather than waited for on the next heartbeat.
    ///
    /// A split changes the routing table for the whole cluster, and up to a
    /// heartbeat interval of clients holding descriptors for a range that no
    /// longer exists is a cost with no corresponding benefit.
    std::function<void(const descriptor_type&, const std::vector<descriptor_type>&)> report_split{};
    std::function<void(const descriptor_type&, const descriptor_type&)> report_merge{};

    /// @brief Heartbeat cadence. Zero disables the tick-driven heartbeat
    ///        entirely, leaving `heartbeat()` for a caller that drives it from
    ///        its own thread.
    std::chrono::milliseconds heartbeat_interval{std::chrono::seconds{10}};

    /// @brief Failure-domain labels for this machine, forwarded in the node
    ///        report. The same vocabulary `quorum_manager` provisions against.
    std::vector<std::string> node_labels{};

    /// @brief Total and available bytes on this machine's storage.
    ///
    /// A hook rather than a `statvfs` call, because the host does not know
    /// which filesystem the state machines actually live on — an object-store
    /// backed engine has no local capacity to report at all, and reporting the
    /// root filesystem's would be worse than reporting nothing.
    std::function<std::pair<std::uint64_t, std::uint64_t>()> capacity_probe{};

    /// @brief The machine is asking not to be given more work.
    ///
    /// Evaluated per heartbeat. Left unset, the host reports "not overloaded",
    /// which is the honest answer for a host with no way to tell.
    std::function<bool()> overload_probe{};
};

// ─────────────────────────────────────────────────────────────────────────────
// The host
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Owns many Raft groups in one process.
///
/// Not copyable and not movable: every group's transport view holds a pointer
/// to this object's shared client and demultiplexer, so its address is part of
/// the running state.
///
/// @tparam Types   A bundle satisfying `raft_types`, describing the *shared*
///                 transport and the per-group components.
/// @tparam Key     The routing key. Must satisfy `shard_key`.
/// @tparam GroupId Must satisfy `raft_group_id`.
template<raft_types Types, shard_key Key, raft_group_id GroupId = std::uint64_t> class multi_raft {
public:
    /// @name Type aliases
    /// @{
    using types = Types;
    using key_type = Key;
    using group_id_type = GroupId;
    using node_id_type = typename Types::node_id_type;
    using term_id_type = typename Types::term_id_type;
    using log_index_type = typename Types::log_index_type;
    using log_entry_type = typename Types::log_entry_type;
    using future_type = typename Types::future_type;

    using shared_client_type = typename Types::network_client_type;
    using shared_server_type = typename Types::network_server_type;

    using messages_type =
        group_rpc_messages<node_id_type, term_id_type, log_index_type, log_entry_type, GroupId>;
    using demux_type = multi_group_network_server<shared_server_type, GroupId, messages_type>;
    using group_types = group_scoped_types<Types, GroupId, demux_type>;
    using group_node_type = node<group_types>;

    using descriptor_type = shard_descriptor<GroupId, Key, node_id_type>;
    using shard_map_type = shard_map<GroupId, Key, node_id_type>;
    using config_type = multi_raft_config<Types, Key, GroupId>;
    using tombstone_set_type = tombstone_set<GroupId>;
    using shard_report_type = shard_report<GroupId, Key, node_id_type>;
    using shard_operation_type = shard_operation<GroupId, Key, node_id_type>;
    using node_report_type = node_report<node_id_type>;
    /// @}

    explicit multi_raft(config_type cfg);
    ~multi_raft();

    multi_raft(const multi_raft&) = delete;
    auto operator=(const multi_raft&) -> multi_raft& = delete;
    multi_raft(multi_raft&&) = delete;
    auto operator=(multi_raft&&) -> multi_raft& = delete;

    /// @name Lifecycle
    /// @{

    /// @brief Start the shared server and every registered group.
    auto start() -> void;

    /// @brief Stop every group, drain the executor, and stop the shared server.
    ///
    /// Synchronous: when it returns there is no joinable thread left and no
    /// group callback is executing. A stop/start/stop sequence is supported,
    /// mirroring the regression `node::stop()` itself guards against.
    ///
    /// Safe against a `tick()` already in flight on another thread: the tick
    /// holds the executor alive for its own duration and then finds it
    /// stopped. It is still the caller's job not to *start* a tick after
    /// `stop()` and expect it to drive anything.
    auto stop() -> void;

    [[nodiscard]] auto is_running() const -> bool;

    /// @brief This host's node identifier, shared by every group's replica.
    [[nodiscard]] auto node_id() const -> const node_id_type&;
    /// @}

    /// @name Group registry
    /// @{

    /// @brief Create a local replica of `descriptor`'s group.
    ///
    /// The replica is assigned a stripe, given a group-scoped store and
    /// transport view, registered with the demultiplexer, and — if the host is
    /// already running — started.
    ///
    /// @throws unknown_shard_exception if the group already exists locally.
    auto create_group(const descriptor_type& descriptor) -> void;

    /// @brief Create a group from its id and voter set, with an unbounded range.
    ///
    /// The bootstrap shape: one group owning `(-inf, +inf)`.
    auto create_group(const GroupId& group, const std::vector<node_id_type>& voters) -> void;

    /// @brief Create a local replica whose store is seeded before the node opens it.
    ///
    /// Split apply needs this: a child begins life at the parent's apply index
    /// with an EMPTY log and a synthetic snapshot that *is* its share of the
    /// parent's state, and `node::start()` reads that snapshot during
    /// `initialize_from_storage()`. Seeding after construction would be too
    /// late.
    auto create_group(const descriptor_type& descriptor,
                      const std::function<void(typename Types::persistence_engine_type&)>& seed)
        -> void;

    /// @brief Tear down a local replica.
    ///
    /// Unregisters the group from the demultiplexer first so no new work can
    /// arrive, drains its stripe, then stops and destroys the node. Teardown
    /// never runs on the destroyed group's own stripe — that stripe is
    /// executing the group.
    ///
    /// @param reason Recorded in the tombstone set, so a stale peer's messages
    ///               for this group are dropped rather than re-creating it.
    auto destroy_group(const GroupId& group, tombstone_reason reason) -> bool;

    [[nodiscard]] auto has_group(const GroupId& group) const -> bool;
    [[nodiscard]] auto group_count() const -> std::size_t;
    [[nodiscard]] auto group_ids() const -> std::vector<GroupId>;

    /// @brief The group's `node`, or `nullptr`. For tests and host-internal use.
    [[nodiscard]] auto group_node(const GroupId& group) -> group_node_type*;
    /// @}

    /// @name Client surface (Requirement 16)
    /// @{

    /// @brief Route `command` by `key` and submit it to the owning shard.
    ///
    /// Resolves the key against the local routing map, checks the resolved
    /// shard's epoch against the local replica's, and submits. The retry loop
    /// handles the two failures a *host* can repair without leaving the
    /// process: a stale routing row (repaired from the local groups'
    /// authoritative descriptors, then retried) and a shard frozen mid-merge
    /// (backed off, then retried).
    ///
    /// **It does not forward to another node.** Kythira's transport carries no
    /// client-command RPC — commands reach a node through whatever the
    /// application already speaks — so a request whose local replica is not
    /// the leader fails with `shard_not_leader_exception` carrying the hint,
    /// and the caller does the hop. That is MicroRaft's contract, surfaced at
    /// the boundary that can actually honour it.
    ///
    /// @throws unrouted_key_exception       no shard owns the key
    /// @throws unknown_shard_exception      no local replica of the owning shard
    /// @throws shard_not_leader_exception   local replica is not the leader
    /// @throws cross_shard_command_exception  the command's own key is elsewhere
    auto submit_command(const Key& key, const std::vector<std::byte>& command,
                        std::chrono::milliseconds timeout) -> future_type;

    /// @brief Submit to a named shard at a named epoch.
    ///
    /// The form a client uses once it holds a descriptor: no key lookup, and
    /// the epoch is checked rather than derived, so a request computed against
    /// a routing table that has since moved is rejected instead of served.
    auto submit_command(const GroupId& group, shard_epoch expected_epoch,
                        const std::vector<std::byte>& command, std::chrono::milliseconds timeout)
        -> future_type;

    /// @brief Submit using the configured partitioner to derive the key.
    ///
    /// @throws std::logic_error if no partitioner is configured.
    auto submit_command(const std::vector<std::byte>& command, std::chrono::milliseconds timeout)
        -> future_type;

    /// @brief Linearisable read against the shard owning `key`.
    auto read_state(const Key& key, std::chrono::milliseconds timeout) -> future_type;

    /// @brief Resolve `key` to a descriptor without submitting anything.
    [[nodiscard]] auto resolve(const Key& key) const -> std::optional<descriptor_type>;
    /// @}

    /// @name Split (Requirement 11, design §5.3 and §5.4)
    /// @{

    /// @brief Propose a split of `group` at `at_keys`. Leader-only.
    ///
    /// Runs design §5.3's sequence: gate, then candidate keys minus the state
    /// machine's vetoes, then a fallback to `suggest_split_keys`, then the id
    /// allocation, then the derived children, then the proposal.
    ///
    /// The returned future resolves when the split entry has been committed and
    /// applied on this replica.
    ///
    /// @param at_keys Empty means "you choose": the state machine's suggestions
    ///                are used, which is how a policy that says "split, I don't
    ///                care where" is served.
    ///
    /// @throws (via the future)
    ///   `shard_busy_exception`             the shard is not `stable`
    ///   `shard_not_leader_exception`       this replica does not lead it
    ///   `split_key_out_of_range_exception` a named key is outside the range
    ///   `no_valid_split_key_exception`     every candidate was vetoed and the
    ///                                      state machine suggested none
    ///   `std::runtime_error`               the id authority was unavailable
    auto split_shard(const GroupId& group, std::vector<Key> at_keys,
                     std::chrono::milliseconds timeout) -> future_type;

    /// @brief Split with explicit operator control (design §6.2).
    auto split_shard(const GroupId& group, std::vector<Key> at_keys, split_options options,
                     std::chrono::milliseconds timeout) -> future_type;

    /// @brief Pre-split an EMPTY shard at `boundaries`.
    ///
    /// For the bulk-load case TiKV RFC 0082 names: "in the very beginning,
    /// writes will only happen in a single region, the problem can be solved by
    /// pre-split + scatter". Refused on a non-empty shard, deliberately —
    /// otherwise it becomes a second, weaker `split_shard` that skips the
    /// state machine's veto.
    auto pre_split(const GroupId& group, std::vector<Key> boundaries,
                   std::chrono::milliseconds timeout) -> future_type;

    /// @brief Remove a shard from the automatic channels.
    ///
    /// Freezing blocks the policy, the state-machine hints and the placement
    /// driver — but NOT explicit admin commands. Freezing an operator out of
    /// their own escape hatch would be a bad joke at 3 a.m.
    auto freeze_shard(const GroupId& group) -> bool;
    auto thaw_shard(const GroupId& group) -> bool;

    /// @brief The global kill switch for the automatic channels.
    auto set_automatic_split_merge_enabled(bool enabled) -> void;
    [[nodiscard]] auto automatic_split_merge_enabled() const -> bool;

    /// @brief Ask the arbiter whether an operation would be admitted, without
    /// starting one. Diagnostic; the real gates run inside the operations.
    [[nodiscard]] auto would_admit(const GroupId& group, signal_channel channel) const
        -> arbiter_decision<GroupId>;

    /// @brief Statistics for one shard, as a policy would see them.
    [[nodiscard]] auto stats_for(const GroupId& group) const
        -> std::optional<shard_stats<GroupId, Key, node_id_type>>;

    /// @brief What channel (c′)'s sampler is doing for one shard.
    ///
    /// Diagnostic, and not a luxury: "no load split happened" is the same
    /// observable outcome for a cold shard, a shard mid-window, a shard whose
    /// spike ended early, and a shard backed off after a single-hot-key
    /// verdict. Only the last is something an operator can act on.
    [[nodiscard]] auto load_sampler_state_of(const GroupId& group) const
        -> std::optional<load_sampler_state>;

    /// @brief The sampler's counters for one shard: windows started, spikes
    ///        abandoned, single-hot-key verdicts, proposals.
    [[nodiscard]] auto load_sampler_counters(const GroupId& group) const
        -> std::optional<std::array<std::uint64_t, 4>>;

    /// @brief How many operations the arbiter has refused, by gate.
    [[nodiscard]] auto rejection_count(arbiter_gate gate) const -> std::uint64_t;

    /// @brief How many merges a policy has vetoed, and by which policy.
    ///
    /// Separately counted from the other refusals because under the composite's
    /// unanimity rule a single member can hold every merge in the cluster
    /// hostage, and the first question an operator asks is *which one*. An
    /// empty key is the unattributed case.
    [[nodiscard]] auto merge_veto_count(const std::string& policy) const -> std::uint64_t;
    [[nodiscard]] auto merge_veto_count() const -> std::uint64_t;

    /// @brief How many operations are in flight across every local shard.
    [[nodiscard]] auto operations_in_flight() const -> std::size_t;

    /// @name Placement driver, channel (d) (Requirement 14, design §7)
    /// @{

    /// @brief Send one batched heartbeat now and act on what comes back.
    ///
    /// Returns the number of operators received. Safe to call from any thread;
    /// `tick()` also calls it on the configured cadence.
    auto heartbeat() -> std::size_t;

    /// @brief One report per shard this host currently **leads**.
    ///
    /// Leader-only, following TiKV. A follower's view of size and load is the
    /// leader's view delayed, so N-1 copies of a stale report would cost
    /// bandwidth to tell the driver nothing it does not already know.
    [[nodiscard]] auto build_shard_reports() const -> std::vector<shard_report_type>;

    /// @brief This machine's own report: capacity, counts, rates, labels.
    [[nodiscard]] auto build_node_report() const -> node_report_type;

    /// @brief Act on one operator, or decline it with a counted reason.
    ///
    /// Every rejection here is a **normal** outcome, not an error: the driver
    /// computed the operator from a report that is at most one heartbeat old,
    /// and one heartbeat is exactly how out of date it is allowed to be. The
    /// driver reissues if it still wants the operator, so nothing needs undoing
    /// and nothing needs queueing.
    auto apply_operator(const shard_operation_type& op) -> operator_outcome;

    /// @brief Move `group`'s leadership to `to`. Requires a TimeoutNow-capable
    ///        transport; see `network_client_with_timeout_now`.
    auto transfer_leadership(const GroupId& group, const node_id_type& to,
                             std::chrono::milliseconds timeout) -> future_type;

    /// @brief Hand `group`'s leadership to some *other* voter.
    ///
    /// The point of a scatter is that the children of a split must not all lead
    /// from the machine that was already the hotspot. The target is chosen
    /// round-robin over the descriptor's voters, from purely local information
    /// — this host knows which groups *it* leads and nothing about anyone
    /// else's leader count, so anything more informed would be a guess dressed
    /// up as a measurement. The placement driver's `transfer_leader` operator
    /// is the informed version; this is the version that works with no control
    /// plane at all.
    auto scatter(const GroupId& group, std::chrono::milliseconds timeout) -> future_type;

    [[nodiscard]] auto heartbeat_count() const -> std::uint64_t;
    [[nodiscard]] auto received_operator_count() const -> std::uint64_t;
    [[nodiscard]] auto accepted_operator_count() const -> std::uint64_t;
    [[nodiscard]] auto skipped_operator_count() const -> std::uint64_t;
    /// @brief How many operators were skipped for one specific reason.
    [[nodiscard]] auto skipped_operator_count(skipped_operator_reason reason) const
        -> std::uint64_t;
    /// @}

    /// @brief Called on every role, term or membership transition.
    ///
    /// MicroRaft's `RaftNodeReportListener`, and the reason it is a push rather
    /// than a poll: an operator watching a thousand groups cannot poll them.
    auto set_report_listener(
        std::function<void(const group_report<GroupId, node_id_type>&)> listener) -> void;

    /// @brief The shard's current operation state, or `nullopt` if not local.
    [[nodiscard]] auto operation_state(const GroupId& group) const
        -> std::optional<shard_operation_state>;

    /// @brief How many splits this node has applied. Test and diagnostic use.
    [[nodiscard]] auto applied_split_count() const -> std::uint64_t;

    /// @brief Merge `source` into `target`. Proposed by the source's leader.
    ///
    /// Runs design §5.5's protocol. This host must lead the source; the target
    /// side is driven by whichever host leads the target, which learns of the
    /// merge by applying the `merge_prepare` entry on its own local source
    /// replica. That works because merge **requires colocated replicas**, so
    /// the machine leading the target necessarily holds one — one fewer channel
    /// than design §5.5's out-of-band notification, and one fewer way for the
    /// two sides to disagree.
    ///
    /// The returned future resolves when `merge_prepare` has been committed and
    /// applied on this replica — that is, when the source is frozen and the
    /// merge is under way. It does **not** wait for the target to commit;
    /// `merge_state()` reports that.
    ///
    /// @throws (via the future)
    ///   `shard_not_adjacent_exception`        the ranges do not touch
    ///   `shard_alignment_required_exception`  the replica sets are not colocated
    ///   `shard_busy_exception`                either shard is not `stable`
    ///   `shard_epoch_mismatch_exception`      either descriptor has moved
    ///   `shard_not_leader_exception`          this replica does not lead the source
    auto merge_shards(const GroupId& source, const GroupId& target,
                      std::chrono::milliseconds timeout) -> future_type;

    /// @brief Abandon a merge this host's source leader started.
    ///
    /// Asks the target to record `merge_abandoned` in its own log; once that is
    /// committed, the source proposes `merge_rollback` and resumes serving. The
    /// target refuses if `merge_commit` is already proposed — commit always
    /// wins, because both decisions are made by the same single log.
    auto abandon_merge(const GroupId& source, std::chrono::milliseconds timeout) -> future_type;

    /// @brief How many merges this node has applied on a target. Diagnostic.
    [[nodiscard]] auto applied_merge_count() const -> std::uint64_t;

    /// @brief How many merges this node has rolled back. Diagnostic.
    [[nodiscard]] auto rolled_back_merge_count() const -> std::uint64_t;

    /// @brief How many times a frozen source has been reported stalled.
    ///
    /// The host publishes this as `merge.stalled{group, target}`.
    [[nodiscard]] auto stalled_merge_report_count() const -> std::uint64_t;

    /// @brief The target a frozen source is merging into, if it is frozen.
    [[nodiscard]] auto merge_target_of(const GroupId& source) const -> std::optional<GroupId>;

    /// @brief Re-apply a committed administration entry, as a crash-recovery
    /// replay does.
    ///
    /// Administration entries are idempotent by contract — that is what makes
    /// split apply's "children first, then the parent" ordering safe without a
    /// store that spans groups — and this is how that contract is exercised
    /// without corrupting a log to reach it. Returns `false` if the group is
    /// not local or the entry is not an administration entry.
    auto replay_admin_entry(const GroupId& group, const log_entry_type& entry, log_index_type index)
        -> bool;
    /// @}

    /// @name Driving
    /// @{

    /// @brief Drive every ready group through the four phases.
    ///
    /// Ordering is TiKV's and matters: persist strictly before send (never
    /// advertise an append you have not durably taken), and apply strictly
    /// after send (a follower's copy is on the wire before the leader spends
    /// time in the state machine).
    auto tick() -> tick_report;

    /// @brief Queue work to run on `group`'s stripe during the next apply phase.
    ///
    /// The apply phase exists because `node<Types>` applies committed entries
    /// inline, on whichever thread drove the commit — so the host's own
    /// per-group work (admin-entry dispatch from Phase 6 onward, policy
    /// follow-ups) needs a place that is already ordered after the send phase.
    auto defer_to_apply_phase(const GroupId& group, std::function<void()> work) -> bool;
    /// @}

    /// @name Hibernation
    /// @{

    /// @brief Take a group out of hibernation. Returns `true` if it was hibernating.
    ///
    /// Wake conditions per Requirement 5.5: any client request, any inbound
    /// RPC, any configuration change, any placement-driver operator. The
    /// inbound-RPC case is wired through the demultiplexer's message observer.
    auto wake(const GroupId& group) -> bool;

    [[nodiscard]] auto is_hibernating(const GroupId& group) const -> bool;
    [[nodiscard]] auto hibernating_count() const -> std::size_t;
    /// @}

    /// @name Routing table and tombstones
    /// @{

    [[nodiscard]] auto shard_map_snapshot() const -> shard_map_type;

    /// @brief Merge descriptors learned from elsewhere into the local map.
    auto learn_descriptors(const std::vector<descriptor_type>& descriptors) -> bool;

    /// @brief Replace a local group's authoritative descriptor *and* publish it.
    ///
    /// The local replica of a group is the authority for that group's row; the
    /// routing map is a cache of it. Split and merge apply call this; nothing
    /// else should.
    auto update_descriptor(const GroupId& group, const descriptor_type& descriptor) -> bool;

    /// @brief Replace a local group's authoritative descriptor *without*
    /// publishing it to the routing map.
    ///
    /// The two halves are separate because split apply performs them at
    /// separate points: the descriptor rows are written durably in step E and
    /// the map is published in step G, with the parent frozen in between. A
    /// caller that publishes early advertises a range whose children are not
    /// durable yet.
    auto set_local_descriptor(const GroupId& group, const descriptor_type& descriptor) -> bool;

    /// @brief Publish a local group's descriptor into the routing map.
    auto publish_descriptor(const GroupId& group) -> bool;

    /// @brief Drop a routing row without touching the replica.
    ///
    /// Merge apply removes the source's row once the survivor owns its range.
    /// A row dropped while the replica is still local is repaired by the next
    /// resolution, which is what makes this safe to call out of order.
    auto forget_routing_row(const GroupId& group) -> bool;

    /// @brief The descriptor the local replica of `group` believes is current.
    [[nodiscard]] auto local_descriptor(const GroupId& group) const
        -> std::optional<descriptor_type>;

    /// @brief Repair the routing map from every local group's own descriptor.
    ///
    /// Returns `true` if anything changed. This is what the routing retry loop
    /// does after an epoch mismatch, and it is why a stale map costs one extra
    /// resolution rather than a control-plane query.
    auto refresh_map_from_local_groups() -> bool;

    /// @brief Commands and reads routed to `group` since it was created.
    [[nodiscard]] auto load_counters(const GroupId& group) const
        -> std::pair<std::uint64_t, std::uint64_t>;

    [[nodiscard]] auto is_tombstoned(const GroupId& group) const -> bool;

    /// @brief Drop tombstones older than the configured horizon.
    auto gc_tombstones(std::chrono::system_clock::time_point now) -> std::size_t;
    /// @}

    /// @name Observability
    /// @{

    /// @brief Messages dropped for a tombstoned or unplaceable group.
    [[nodiscard]] auto stale_group_message_count() const -> std::uint64_t;

    [[nodiscard]] auto executor_stripe_count() const -> std::size_t;

    /// @brief Which stripe a group runs on. `npos` if the group is unknown.
    [[nodiscard]] auto stripe_of(const GroupId& group) const -> std::size_t;

    /// @brief Override the default unknown-group behaviour.
    ///
    /// The default is `lazy_replica_creation` (Requirement 12): tombstone check
    /// in the transport, then the local routing map, then one rate-limited
    /// external lookup, then a replica if and only if this node is a listed
    /// member. Replacing it replaces all of that.
    auto set_unknown_group_handler(std::function<unknown_group_action(const GroupId&)> handler)
        -> void;

    /// @brief How many replicas were created by the lazy path.
    [[nodiscard]] auto lazily_created_replica_count() const -> std::uint64_t;

    /// @brief How many external descriptor lookups the lazy path has made.
    ///
    /// The rate limit is the point: an unknown group id arriving at message
    /// rate must not become a control-plane query at message rate.
    [[nodiscard]] auto descriptor_lookup_count() const -> std::uint64_t;

    /// @brief Requirement 12's decision for a message with no local replica.
    ///
    /// This is the callback the transport installs; it is public because it is
    /// the whole of the lazy-creation policy and a caller replacing it wants to
    /// be able to delegate back to it.
    ///
    /// It does **not** consult the tombstone set: that check lives upstream in
    /// the transport, deliberately, so that a tombstoned group's message never
    /// reaches replica creation at all.
    auto handle_unknown_group(const GroupId& group) -> unknown_group_action;
    /// @}

private:
    /// @brief Everything the host owns about one group.
    ///
    /// Held by `shared_ptr` so that a tick phase already holding a reference is
    /// unaffected by a concurrent `destroy_group()` removing it from the
    /// registry — the node is torn down only after the stripe drains, and the
    /// last reference goes with it.
    struct group_state {
        /// @param latency_windows Sub-windows in each latency digest's ring;
        ///        see `multi_raft_config::latency_window_count`.
        explicit group_state(std::size_t latency_windows = latency_digest::k_default_windows,
                             load_split_sampler_config sampler = {})
            : _load_sampler(sampler),
              _read_latency(latency_windows),
              _apply_latency(latency_windows) {}

        GroupId _group_id{};
        std::size_t _stripe{0};
        std::unique_ptr<group_node_type> _node;
        descriptor_type _descriptor{};

        /// Wall-free idle clock: hibernation is a local scheduling decision and
        /// must not acquire a dependency on synchronised clocks.
        std::atomic<std::int64_t> _last_activity_ns{0};
        std::atomic<bool> _hibernating{false};

        mutable std::mutex _deferred_mutex;
        std::vector<std::function<void()>> _deferred;

        /// Load counters, read by the policy channel from Phase 9 onward.
        ///
        /// Cumulative. The *rates* below are derived from them on the policy
        /// tick, which is the only place with two observations and the interval
        /// between them.
        std::atomic<std::uint64_t> _reads{0};
        std::atomic<std::uint64_t> _writes{0};
        std::atomic<std::uint64_t> _read_bytes{0};
        std::atomic<std::uint64_t> _write_bytes{0};

        /// Rates as of the last policy tick, in per-second units. Published in
        /// `shard_stats` and used as the sampler's entry test — a cumulative
        /// count cannot be compared against a threshold expressed in QPS.
        std::atomic<double> _read_qps{0.0};
        std::atomic<double> _write_qps{0.0};
        std::atomic<double> _read_bytes_per_sec{0.0};
        std::atomic<double> _write_bytes_per_sec{0.0};

        /// Previous observation, for the rate derivation. Touched only on the
        /// policy tick, which runs on this group's stripe.
        std::uint64_t _prev_reads{0};
        std::uint64_t _prev_writes{0};
        std::uint64_t _prev_read_bytes{0};
        std::uint64_t _prev_write_bytes{0};
        std::chrono::steady_clock::time_point _rate_sampled_at{};

        /// @brief The load sampler and the hot keys it last produced.
        ///
        /// The sampler is not thread-safe by design — it holds a reservoir and
        /// a state machine, and a lock inside it would put one on the request
        /// path. The host takes `_sampler_mutex` around it instead, and only
        /// when sampling is configured on, so the disabled case never
        /// contends.
        mutable std::mutex _sampler_mutex;
        load_split_sampler<Key> _load_sampler;
        std::vector<hot_key_sample<Key>> _hot_keys;

        /// @brief Latency percentiles over a bounded recent window (design
        ///        §6.1.4), rotated on the policy tick.
        ///
        /// Two digests of the same type and window, so `_p99_read_latency` and
        /// `_p99_apply_latency` cannot come to mean different things.
        ///
        /// By value: `latency_digest` owns atomics and is neither copyable nor
        /// movable, and `group_state` is built once with `make_shared` and only
        /// ever handed around by that pointer, so it never needs to be either.
        latency_digest _read_latency;
        latency_digest _apply_latency;

        /// @brief The arbiter's per-shard state (design §6.6).
        ///
        /// Only `stable` admits a new operation, so a conflicting split and
        /// merge are impossible by construction rather than by check-then-act.
        /// The full transition table and the remaining gates land with the
        /// arbiter in Phase 9; the state itself lives here from Phase 7 because
        /// split already needs it.
        std::atomic<shard_operation_state> _operation{shard_operation_state::stable};

        // ── merge bookkeeping (design §5.5) ──────────────────────────────────
        //
        // Guarded by `_merge_mutex` rather than atomics: these move together,
        // and a half-updated merge record is exactly the state that lets a
        // frozen source resume while a target replica has already committed.
        mutable std::mutex _merge_mutex;
        /// Set on a frozen source: the target it is merging into.
        std::optional<GroupId> _merge_target;
        /// Set on a target that has accepted a merge: the source it is taking.
        std::optional<GroupId> _merge_source;
        /// The index `merge_prepare` was applied at on a frozen source.
        log_index_type _merge_prepare_index{0};
        /// The lowest index every source voter was known to hold at prepare.
        log_index_type _merge_min_index{0};
        /// Set once this target has proposed `merge_commit`: from then on it
        /// refuses to abandon. Commit always wins.
        bool _merge_commit_proposed{false};
        /// Set once this target has committed `merge_abandoned`: from then on
        /// it refuses to commit, and a new target leader replaying the entry
        /// inherits the refusal.
        bool _merge_abandoned{false};
        /// When the source was frozen, for the stall warning.
        std::chrono::steady_clock::time_point _merge_frozen_at{};

        // ── arbiter history (the anti-oscillation inputs) ────────────────────
        std::atomic<std::int64_t> _last_split_ns{0};
        std::atomic<std::int64_t> _last_merge_ns{0};
        std::atomic<std::int64_t> _leader_since_ns{0};
        /// Set by `freeze_shard`. Distinct from the operation state, because a
        /// frozen shard that starts an admin operation must return to FROZEN,
        /// not to stable.
        std::atomic<bool> _frozen{false};
    };

    using group_ptr = std::shared_ptr<group_state>;

    // ── internals ────────────────────────────────────────────────────────────

    [[nodiscard]] auto find_group(const GroupId& group) const -> group_ptr;
    [[nodiscard]] auto all_groups() const -> std::vector<group_ptr>;

    auto note_activity(const GroupId& group) -> void;
    auto note_activity(group_state& g) -> void;

    /// @brief Run `fn` for every group in `ready`, each on its own stripe,
    /// and block until all of them have finished.
    ///
    /// One barrier per phase across groups, with per-group serialisation
    /// inside it. Posting all of them and waiting once is what keeps a phase's
    /// wall time proportional to the slowest group rather than to their sum.
    auto run_phase(const std::vector<group_ptr>& ready, const std::function<void(group_state&)>& fn)
        -> void;

    [[nodiscard]] auto select_ready(std::vector<group_ptr>& hibernating_out)
        -> std::vector<group_ptr>;

    auto evaluate_hibernation(const std::vector<group_ptr>& groups) -> void;
    [[nodiscard]] auto hibernation_enabled(std::size_t group_count) const -> bool;
    [[nodiscard]] auto leader_hibernate_after() const -> std::chrono::nanoseconds;
    [[nodiscard]] auto follower_hibernate_after() const -> std::chrono::nanoseconds;

    auto persist_tombstones() -> void;

    /// @brief A reference to the current pool that stays valid while held.
    [[nodiscard]] auto current_executor() const -> std::shared_ptr<striped_serial_executor>;

    // ── split ────────────────────────────────────────────────────────────────

    using split_command_type = split_command<GroupId, Key, node_id_type>;
    using snapshot_type = typename Types::snapshot_type;

    /// @brief Install the administration-entry handler on a group's node.
    auto install_admin_handler(group_state& g) -> void;

    /// @brief Design §5.4 steps A-J, on every replica, inside the apply loop.
    ///
    /// Runs with the parent node's mutex held and with the parent's state
    /// machine by reference — which is why it never calls back into that node.
    auto apply_split(group_state& parent, const split_command_type& cmd, log_index_type at_index,
                     term_id_type at_term, typename Types::state_machine_type& parent_sm) -> void;

    using merge_prepare_command_type = merge_prepare_command<GroupId, Key, node_id_type>;
    using merge_commit_command_type =
        merge_commit_command<GroupId, Key, node_id_type, log_entry_type>;
    using merge_rollback_command_type = merge_rollback_command<GroupId>;
    using merge_abandoned_command_type = merge_abandoned_command<GroupId>;

    /// @brief Design §5.5's source-side apply: freeze, and notify by committing.
    auto apply_merge_prepare(group_state& source, const merge_prepare_command_type& cmd,
                             log_index_type at_index) -> void;

    /// @brief Design §5.5's target-side apply, on every target replica.
    auto apply_merge_commit(group_state& target, const merge_commit_command_type& cmd,
                            typename Types::state_machine_type& target_sm) -> void;

    auto apply_merge_rollback(group_state& source, const merge_rollback_command_type& cmd) -> void;
    auto apply_merge_abandoned(group_state& target, const merge_abandoned_command_type& cmd)
        -> void;

    /// @brief Propose `merge_commit` if this host leads `target`.
    ///
    /// Called after a `merge_prepare` is applied on the local source replica.
    /// The prepare entry IS the notification; see `merge_shards`.
    auto maybe_propose_merge_commit(const merge_prepare_command_type& cmd) -> void;

    /// @brief Every precondition from design §5.5's table, in one place.
    ///
    /// Checked at proposal AND re-checked at apply, because an entry proposed
    /// under one epoch can commit after the epoch moved.
    [[nodiscard]] auto check_merge_preconditions(const descriptor_type& source,
                                                 const descriptor_type& target,
                                                 const group_ptr& source_state,
                                                 const group_ptr& target_state) const
        -> std::exception_ptr;

    /// @brief Every gate from design §6.6's table, in one place.
    ///
    /// The state gate is a *transition*, not a check: `admit` claims the shard
    /// by compare-exchange, so two channels racing in the same interval cannot
    /// both proceed. `release` puts it back.
    [[nodiscard]] auto admit(group_state& g, signal_channel channel, shard_operation_state to,
                             bool override_cooldown) -> arbiter_decision<GroupId>;
    auto release(group_state& g) -> void;
    auto note_rejection(const GroupId& group, arbiter_gate gate, signal_channel channel,
                        const char* operation) -> void;

    /// @brief The `policy` metric dimension, with a name for "nobody said".
    ///
    /// A single configured policy that never named itself is not anonymous by
    /// accident — it is the ordinary case — so it gets a value rather than a
    /// missing dimension, which would make the two situations
    /// indistinguishable in a query.
    [[nodiscard]] static auto policy_label(const char* policy) -> std::string {
        return policy == nullptr ? std::string{"unattributed"} : std::string{policy};
    }
    auto publish_report(group_state& g) -> void;
    auto note_merge_veto(const GroupId& group, const char* policy, merge_reason reason) -> void;

    /// @brief Turn the cumulative load counters into per-second rates, and
    ///        advance channel (c′)'s sampler off them.
    ///
    /// Called once per policy tick, which is the only place holding two
    /// observations and the interval between them.
    auto update_load_rates(group_state& g, std::chrono::steady_clock::time_point now) -> void;

    /// @brief Channel (a): ask the policy about every local leader, once per
    /// `policy_interval`.
    auto evaluate_policy(const std::vector<group_ptr>& ready) -> void;

    /// @brief Channel (d): heartbeat on the configured cadence, from `tick()`.
    auto maybe_heartbeat() -> void;

    auto note_skipped_operator(const shard_operation_type& op, skipped_operator_reason reason)
        -> operator_outcome;

    /// @brief The configured key codec, or the default, as one object.
    [[nodiscard]] auto make_key_codec() const -> key_codec_adapter<Key>;

    [[nodiscard]] auto encode_key_or_default(const Key& k) const -> std::vector<std::byte>;
    [[nodiscard]] auto decode_key_or_default(const std::vector<std::byte>& b) const -> Key;

    /// @brief Shared body of the routing retry loop.
    ///
    /// `resolve_key` is `nullopt` for the group-addressed form, which skips the
    /// key lookup but keeps every other gate.
    auto route_and_run(std::optional<Key> key, std::optional<GroupId> group,
                       std::optional<shard_epoch> expected_epoch,
                       const std::vector<std::byte>* command, bool is_read,
                       std::chrono::milliseconds timeout) -> future_type;

    [[nodiscard]] static auto failed_future(std::exception_ptr error) -> future_type;

    [[nodiscard]] static auto now_ns() -> std::int64_t;

    // ── members ──────────────────────────────────────────────────────────────

    config_type _cfg;
    shared_client_type _client;
    demux_type _demux;

    /// Stripe count is fixed for the host's lifetime so a group's stripe index
    /// stays valid across a stop/start cycle.
    std::size_t _stripe_count{1};
    /// Recreated by `start()` and released by `stop()`.
    ///
    /// `stop()` has to leave no joinable thread behind — the same contract
    /// `node::stop()` carries, and `node` has no destructor to fall back on —
    /// but a `striped_serial_executor` cannot be restarted once its threads are
    /// joined. Holding it by pointer is what lets `stop()` honour both halves:
    /// join everything, then build a fresh pool on the next `start()`.
    ///
    /// `shared_ptr`, not `unique_ptr`, and every reader takes a copy under
    /// `_executor_mutex`. A `tick()` running concurrently with `stop()` is
    /// plausible — a driver thread is mid-tick when an operator stops the host
    /// — and with a bare pointer that tick dereferences a pool `stop()` has
    /// already destroyed. A copied `shared_ptr` keeps the pool alive for the
    /// tick's duration; the tick then finds it stopped, its `post()` calls
    /// fail, and it falls back to running inline.
    mutable std::mutex _executor_mutex;
    std::shared_ptr<striped_serial_executor> _executor;

    mutable std::shared_mutex _registry_mutex;
    std::unordered_map<GroupId, group_ptr> _groups;

    mutable std::shared_mutex _map_mutex;
    shard_map_type _shard_map;

    mutable std::mutex _tombstone_mutex;
    tombstone_set_type _tombstones;

    std::atomic<bool> _running{false};
    std::chrono::steady_clock::time_point _last_policy_run{};
    std::atomic<std::uint64_t> _applied_splits{0};
    std::atomic<std::uint64_t> _applied_merges{0};
    std::atomic<std::uint64_t> _rolled_back_merges{0};
    std::atomic<std::uint64_t> _stalled_merges{0};

    std::atomic<bool> _automatic_enabled{true};
    std::chrono::steady_clock::time_point _last_heartbeat{};
    std::atomic<std::uint64_t> _heartbeats{0};
    std::atomic<std::uint64_t> _operators_received{0};
    std::atomic<std::uint64_t> _operators_accepted{0};
    std::atomic<std::uint64_t> _operators_skipped{0};
    mutable std::mutex _skip_mutex;
    std::unordered_map<std::uint8_t, std::uint64_t> _operator_skips;
    /// Round-robin cursor for `scatter`. Local, and deliberately so: it is what
    /// makes two consecutive scatters on this host pick different targets, and
    /// nothing more is claimed for it.
    std::atomic<std::uint64_t> _scatter_cursor{0};
    mutable std::mutex _rejection_mutex;
    std::unordered_map<std::uint8_t, std::uint64_t> _rejections;
    std::unordered_map<std::string, std::uint64_t> _merge_vetoes;
    std::function<void(const group_report<GroupId, node_id_type>&)> _report_listener;
    std::atomic<std::uint64_t> _lazy_replicas{0};
    std::atomic<std::uint64_t> _descriptor_lookups{0};

    /// When each group id was last looked up externally, so the lazy path
    /// cannot turn a message flood into a control-plane flood.
    mutable std::mutex _lookup_mutex;
    std::unordered_map<GroupId, std::chrono::steady_clock::time_point> _last_lookup;
};

}  // namespace kythira
