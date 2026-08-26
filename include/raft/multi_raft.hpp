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
#include <raft/raft.hpp>
#include <raft/shard_exceptions.hpp>
#include <raft/shard_map.hpp>
#include <raft/shard_types.hpp>
#include <raft/striped_executor.hpp>

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

    /// One barrier per tick instead of one per ready group; see the type's docs.
    std::optional<tick_batch_controller> batch_controller{};

    /// Where the host's own durable state lives (shard map, tombstones).
    /// Empty means the host keeps them in memory only.
    std::filesystem::path host_data_dir{};

    /// How long a tombstone is kept before garbage collection.
    std::chrono::milliseconds tombstone_horizon{std::chrono::hours{24}};
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
    auto stop() -> void;

    [[nodiscard]] auto is_running() const -> bool;
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

    /// @brief Called for a message whose group has no local replica.
    ///
    /// Phase 7 (task 19) replaces the default with lazy replica creation; until
    /// then the default drops, which is the safe answer.
    auto set_unknown_group_handler(std::function<unknown_group_action(const GroupId&)> handler)
        -> void;
    /// @}

private:
    /// @brief Everything the host owns about one group.
    ///
    /// Held by `shared_ptr` so that a tick phase already holding a reference is
    /// unaffected by a concurrent `destroy_group()` removing it from the
    /// registry — the node is torn down only after the stripe drains, and the
    /// last reference goes with it.
    struct group_state {
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
        std::atomic<std::uint64_t> _reads{0};
        std::atomic<std::uint64_t> _writes{0};
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
    std::unique_ptr<striped_serial_executor> _executor;

    mutable std::shared_mutex _registry_mutex;
    std::unordered_map<GroupId, group_ptr> _groups;

    mutable std::shared_mutex _map_mutex;
    shard_map_type _shard_map;

    mutable std::mutex _tombstone_mutex;
    tombstone_set_type _tombstones;

    std::atomic<bool> _running{false};
    std::chrono::steady_clock::time_point _last_policy_run{};
};

}  // namespace kythira
