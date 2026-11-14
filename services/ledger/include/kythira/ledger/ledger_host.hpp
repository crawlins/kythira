// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The ledger node's core over multi_raft (.kiro/specs/verifiable-ledger/
// task 7c, Requirements 1.1, 2.3, 2.4, 3.5, 6.2, 10.4): everything
// ledger_node does between the HTTPS server and the Raft groups, with no
// transport or configuration file of its own, so the in-process tests run
// it on a message fabric and the daemon runs it on the network.
//
//  - Groups. The catalog is group catalog_group, on every member. A new
//    ledger's group is placed on `replication` members (round robin over
//    the member list) by the catalog leader, and the catalog assigns its
//    group id at apply (ledger_catalog.hpp), so every node reads the same
//    placement. Each node creates the groups it is a voter of when it sees
//    them in its catalog replica, and destroys a purged ledger's group.
//    reconcile() does this; the maintenance thread runs it every pass.
//  - Backend. basic_ledger_host is ledger_api's Backend: catalog reads are
//    served from the local catalog replica, except that only the catalog
//    leader says a ledger does not exist (a follower may not have applied
//    its create yet, so it redirects); writes and every ledger route
//    go to the group's leader, and a member that is not the leader answers
//    not_leader with the leader's API URL (or a voter's, when it is not a
//    replica at all). A ledger's sessions are tied to its group's term. A
//    new leader passes a read-index barrier before its first request of
//    the term, so prepare() sees every entry committed before it.
//  - Maintenance. On ledgers it leads, the maintenance thread proposes
//    index build steps for indexes still building, a seal once the
//    unsealed journal reaches the segment size (when the ledger has a
//    segment directory), and runs export jobs still in progress through
//    `run_export`, proposing their finish entry. Proposals block until
//    applied, so maintenance runs on its own thread, never on one that
//    drives multi_raft's tick.
//
// The multi_raft config must build its state machines from context():
//
//   auto ctx = host_type::make_context(options_fn);
//   cfg.state_machine_factory = [ctx](const auto&) { return group_sm(ctx); };
//   cfg.automatic_split_merge_enabled = false;  // ledger groups never move
//
// Node ids (integers or strings, as kythira::host_node_id is built) are
// written as text in the catalog: integers in decimal.

#include <kythira/ledger/journal_export.hpp>
#include <kythira/ledger/journal_segments.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/ledger_api.hpp>
#include <kythira/ledger/ledger_catalog.hpp>
#include <kythira/ledger/ledger_group_state_machine.hpp>
#include <kythira/ledger/ledger_sessions.hpp>
#include <kythira/ledger/ledger_state_machine.hpp>

#include <raft/completion_exceptions.hpp>
#include <raft/group_storage.hpp>
#include <raft/shard_exceptions.hpp>
#include <raft/shard_types.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace kythira::ledger {

/// Counters for the node's metrics.
struct ledger_host_stats {
    std::uint64_t groups_created = 0;
    std::uint64_t groups_destroyed = 0;
    std::uint64_t index_steps = 0;
    std::uint64_t seals = 0;
    std::uint64_t exports_finished = 0;
    std::uint64_t maintenance_errors = 0;
    std::uint64_t split_merge_vetoes = 0;
};

template<typename Host, kythira::kv_engine Engine> class basic_ledger_host {
public:
    using host_type = Host;
    using engine_type = Engine;
    using node_id_type = typename Host::node_id_type;
    using descriptor_type = typename Host::descriptor_type;
    using group_id_type = std::remove_cvref_t<decltype(std::declval<descriptor_type>()._group_id)>;
    using group_sm = basic_ledger_group_state_machine<Engine>;
    using ledger_sm = typename group_sm::ledger_type;
    using catalog_sm = typename group_sm::catalog_type;
    using handle_type = ledger_handle<ledger_sm>;

    static_assert(std::integral<node_id_type> || std::same_as<node_id_type, std::string>,
                  "ledger_host writes node ids as decimal integers or strings");

    /// A node id as the catalog writes it, and back.
    static auto id_text(const node_id_type& id) -> std::string {
        if constexpr (std::integral<node_id_type>) {
            return std::to_string(id);
        } else {
            return id;
        }
    }
    static auto id_from_text(const std::string& text) -> node_id_type {
        if constexpr (std::integral<node_id_type>) {
            return static_cast<node_id_type>(std::stoull(text));
        } else {
            return text;
        }
    }
    static_assert(std::integral<group_id_type>, "the catalog assigns integer group ids");

    struct options {
        /// Every member, this node included: the catalog's voters and the
        /// pool ledgers are placed on.
        std::vector<node_id_type> members;
        /// Voters per ledger group (all members when there are fewer).
        std::size_t replication = 3;
        /// Each member's API base URL, for not_leader redirects.
        std::map<node_id_type, std::string> api_urls;
        std::chrono::milliseconds propose_timeout{5000};
        /// Pause between maintenance passes.
        std::chrono::milliseconds maintenance_interval{200};
        /// Documents each index build step indexes.
        std::uint64_t index_build_step = 256;
        /// Writes an export job's objects (export_journal with the node's
        /// object store). Unset: export jobs finish as failed.
        std::function<export_outcome(const ledger_sm&, const export_job&)> run_export;
        /// Wall clock, ms since the epoch.
        std::function<std::int64_t()> now_ms;
        /// Added to every group id the catalog names (catalog_group and
        /// each ledger's) to give the host's id: 0 stand-alone, the first
        /// id of the ledger module's group space in a composite node
        /// (.kiro/specs/service-colocation/ Requirement 4.1). The catalog
        /// itself stores the ledger's own ids, which do not change.
        group_id_type group_base = 0;
    };

    /// The host's group id for the catalog's group id `local`.
    [[nodiscard]] auto host_group(std::uint64_t local) const -> group_id_type {
        return opts_.group_base + static_cast<group_id_type>(local);
    }

    /// The context every group state machine on this node is built from;
    /// `ledger_options_for` gives a ledger its options (segment directory).
    static auto make_context(
        std::function<ledger_options(const std::string&)> ledger_options_for = {})
        -> std::shared_ptr<typename group_sm::context> {
        auto ctx = std::make_shared<typename group_sm::context>();
        ctx->options = std::move(ledger_options_for);
        return ctx;
    }

    basic_ledger_host(Host& host, std::shared_ptr<typename group_sm::context> ctx, options opts)
        : host_(&host), ctx_(std::move(ctx)), opts_(std::move(opts)) {
        if (!opts_.now_ms) {
            opts_.now_ms = [] {
                return std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                    .count();
            };
        }
    }

    basic_ledger_host(const basic_ledger_host&) = delete;
    auto operator=(const basic_ledger_host&) -> basic_ledger_host& = delete;
    basic_ledger_host(basic_ledger_host&&) = delete;
    auto operator=(basic_ledger_host&&) -> basic_ledger_host& = delete;

    ~basic_ledger_host() { stop(); }

    // ── Lifecycle ───────────────────────────────────────────────────────────

    /// Create the catalog group here (every member does, at every start),
    /// then the ledger groups the catalog replica already names.
    auto bootstrap() -> void {
        const auto cat_group = host_group(catalog_group);
        if (!host_->has_group(cat_group)) {
            host_->create_group(descriptor_for(cat_group, catalog_range(), opts_.members));
        }
        reconcile();
    }

    /// Start the maintenance thread.
    auto start() -> void {
        const std::lock_guard lock(run_mutex_);
        if (worker_.joinable()) {
            return;
        }
        stopping_ = false;
        worker_ = std::thread([this] { maintenance_loop(); });
    }

    auto stop() -> void {
        {
            const std::lock_guard lock(run_mutex_);
            stopping_ = true;
        }
        run_cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    /// Create the ledger groups this node is a voter of and does not have
    /// yet, and destroy purged ledgers' groups.
    auto reconcile() -> void {
        const auto cat = catalog_if_open();
        if (!cat) {
            return;
        }
        const std::string self = id_text(host_->node_id());
        for (const auto& r : cat->list()) {
            if (!r.group) {
                continue;
            }
            const auto g = host_group(*r.group);
            if (r.state == "purged") {
                if (host_->has_group(g) &&
                    host_->destroy_group(g, kythira::tombstone_reason::admin)) {
                    count(&ledger_host_stats::groups_destroyed);
                }
                const std::lock_guard lock(mutex_);
                hosted_.erase(r.ledger_id);
                continue;
            }
            if (std::ranges::find(r.voters, self) == r.voters.end() || host_->has_group(g)) {
                continue;
            }
            std::vector<node_id_type> voters;
            for (const auto& v : r.voters) {
                voters.push_back(id_from_text(v));
            }
            try {
                host_->create_group(descriptor_for(g, ledger_range(r.ledger_id), voters));
                count(&ledger_host_stats::groups_created);
            } catch (const kythira::shard_exception&) {
                // Tombstoned or raced with another reconcile: nothing to do.
            }
        }
    }

    // ── ledger_api Backend ──────────────────────────────────────────────────

    [[nodiscard]] auto catalog() const -> const catalog_sm& {
        auto cat = catalog_if_open();
        if (!cat) {
            throw ledger_not_leader(std::nullopt);
        }
        return *cat;  // kept alive by the context's registry
    }

    auto propose_catalog(std::vector<std::byte> entry) -> std::vector<std::byte> {
        return propose(host_group(catalog_group), std::move(entry));
    }

    /// `replication` members, starting one further along the member list
    /// for each ledger the catalog already has.
    auto place_ledger(const std::string& /*name*/) -> std::vector<std::string> {
        std::vector<node_id_type> members = opts_.members;
        std::ranges::sort(members);
        const std::size_t n = std::min(opts_.replication, members.size());
        const std::size_t offset = members.empty() ? 0 : catalog().list().size() % members.size();
        std::vector<std::string> out;
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(id_text(members[(offset + i) % members.size()]));
        }
        return out;
    }

    auto open(const ledger_record& r) -> std::optional<handle_type> {
        if (!r.group) {
            return std::nullopt;
        }
        const auto g = host_group(*r.group);
        auto* node = host_->group_node(g);
        if (node == nullptr) {
            // Not a replica: send the client to a voter, which knows more.
            std::optional<std::string> url;
            for (const auto& v : r.voters) {
                if (auto u = url_of(id_from_text(v))) {
                    url = std::move(u);
                    break;
                }
            }
            throw ledger_not_leader(std::move(url));
        }
        if (!node->is_leader()) {
            const auto leader = node->known_leader();
            throw ledger_not_leader(leader ? url_of(*leader) : std::nullopt);
        }
        ensure_caught_up(g, *node);
        auto h = hosted_for(r);
        if (!h) {
            throw ledger_not_leader(std::nullopt);  // the group is still opening
        }
        handle_type out;
        out.sm = h->sm.get();
        out.sessions = h->sessions.get();
        out.commit_mutex = &h->commit_mutex;
        out.propose = [this, g](std::vector<std::byte> cmd) { return propose(g, std::move(cmd)); };
        out.start_export = [this](const export_job&) { run_cv_.notify_all(); };
        out.term = [this, g]() -> std::uint64_t {
            auto* n = host_->group_node(g);
            return n == nullptr ? 0U : static_cast<std::uint64_t>(n->get_current_term());
        };
        return out;
    }

    /// A ledger missing from this catalog replica may be one it has not
    /// applied yet: only the catalog leader answers ledger_not_found.
    auto confirm_missing(const std::string& /*name*/) -> void {
        auto* node = host_->group_node(host_group(catalog_group));
        if (node != nullptr && node->is_leader()) {
            return;
        }
        const auto leader = node == nullptr ? std::nullopt : node->known_leader();
        throw ledger_not_leader(leader ? url_of(*leader) : std::nullopt);
    }

    [[nodiscard]] auto term() const -> std::uint64_t { return 0; }  // handles carry their own
    auto now_ms() -> std::int64_t { return opts_.now_ms(); }

    // ── For the node ────────────────────────────────────────────────────────

    [[nodiscard]] auto stats() const -> ledger_host_stats {
        const std::lock_guard lock(mutex_);
        auto s = stats_;
        s.split_merge_vetoes = ctx_->vetoes.load(std::memory_order_relaxed);
        return s;
    }

    /// The ledger state machine open here for `ledger_id`, if any.
    [[nodiscard]] auto ledger(const std::string& ledger_id) const -> std::shared_ptr<ledger_sm> {
        const auto found = ctx_->find(ledger_range(ledger_id));
        if (!found) {
            return nullptr;
        }
        const auto* p = std::get_if<std::shared_ptr<ledger_sm>>(&*found);
        return p == nullptr ? nullptr : *p;
    }

    /// One maintenance pass (the thread calls it; tests may too).
    auto maintain() -> void {
        reconcile();
        const auto cat = catalog_if_open();
        if (!cat) {
            return;
        }
        for (const auto& r : cat->list()) {
            if (r.state != "active" || !r.group) {
                continue;
            }
            const auto g = host_group(*r.group);
            auto* node = host_->group_node(g);
            if (node == nullptr || !node->is_leader()) {
                continue;
            }
            auto h = hosted_for(r);
            if (!h) {
                continue;
            }
            try {
                maintain_ledger(g, *h->sm);
            } catch (const std::exception&) {
                // A lost leadership or a timeout: the next pass, or the
                // next leader, picks it up.
                count(&ledger_host_stats::maintenance_errors);
            }
        }
    }

private:
    struct hosted {
        std::shared_ptr<ledger_sm> sm;
        std::unique_ptr<ledger_session_manager<ledger_sm>> sessions;
        std::mutex commit_mutex;
    };

    static auto descriptor_for(group_id_type g, const kythira::key_range& range,
                               const std::vector<node_id_type>& voters) -> descriptor_type {
        descriptor_type d{};
        d._group_id = g;
        d._range = kythira::shard_range<std::string>{range.start, range.end};
        d._epoch = kythira::shard_epoch{};
        d._voters = voters;
        return d;
    }

    [[nodiscard]] auto catalog_if_open() const -> std::shared_ptr<catalog_sm> {
        const auto found = ctx_->find(catalog_range());
        if (!found) {
            return nullptr;
        }
        const auto* p = std::get_if<std::shared_ptr<catalog_sm>>(&*found);
        return p == nullptr ? nullptr : *p;
    }

    [[nodiscard]] auto url_of(node_id_type id) const -> std::optional<std::string> {
        const auto it = opts_.api_urls.find(id);
        return it == opts_.api_urls.end() ? std::nullopt : std::optional<std::string>(it->second);
    }

    /// This ledger's sessions and commit lock, made on first use; null
    /// while its state machine is not open here.
    auto hosted_for(const ledger_record& r) -> std::shared_ptr<hosted> {
        auto sm = ledger(r.ledger_id);
        const std::lock_guard lock(mutex_);
        auto& slot = hosted_[r.ledger_id];
        if (!sm) {
            return slot;
        }
        if (!slot || slot->sm != sm) {
            slot = std::make_shared<hosted>();
            slot->sm = sm;
            slot->sessions = std::make_unique<ledger_session_manager<ledger_sm>>(*sm);
        }
        return slot;
    }

    /// A new leader may hold committed entries it has not applied yet, and
    /// prepare() must see them (a table created just before the election,
    /// say). Once per term, pass a read-index barrier, which returns once
    /// the state machine has applied everything committed before it.
    template<typename Node> auto ensure_caught_up(group_id_type g, Node& node) -> void {
        const auto term = static_cast<std::uint64_t>(node.get_current_term());
        {
            const std::lock_guard lock(mutex_);
            if (const auto it = ready_term_.find(g);
                it != ready_term_.end() && it->second == term) {
                return;
            }
        }
        try {
            std::ignore = node.read_state(opts_.propose_timeout).get();
        } catch (const std::exception&) {
            throw ledger_not_leader(std::nullopt);
        }
        const std::lock_guard lock(mutex_);
        ready_term_[g] = term;
    }

    /// Propose `cmd` to group `g` here and wait for its apply result.
    auto propose(group_id_type g, std::vector<std::byte> cmd) -> std::vector<std::byte> {
        using not_leader = kythira::shard_not_leader_exception<group_id_type, node_id_type>;
        try {
            auto f = host_->submit_command(g, kythira::shard_epoch{}, cmd, opts_.propose_timeout);
            return std::move(f).get();
        } catch (const not_leader& e) {
            throw ledger_not_leader(e.leader_hint() ? url_of(*e.leader_hint()) : std::nullopt);
        } catch (const kythira::shard_exception&) {
            throw ledger_not_leader(std::nullopt);
        } catch (const kythira::raft_completion_exception& e) {
            // A commit timeout or a lost leadership: the entry may still
            // commit, so the client must not resend it blindly.
            throw ledger_unavailable("commit_unconfirmed", e.what(), false);
        }
    }

    auto propose_bytes(group_id_type g, const std::vector<std::uint8_t>& bytes)
        -> std::vector<std::byte> {
        std::vector<std::byte> cmd(bytes.size());
        std::ranges::transform(bytes, cmd.begin(), [](std::uint8_t b) { return std::byte{b}; });
        return propose(g, std::move(cmd));
    }

    auto maintain_ledger(group_id_type g, const ledger_sm& sm) -> void {
        for (const auto& t : sm.tables()) {
            for (const auto& idx : sm.indexes(t.name)) {
                if (!idx.active) {
                    (void)propose_bytes(
                        g, encode_index_build({t.name, idx.name, opts_.index_build_step}));
                    count(&ledger_host_stats::index_steps);
                }
            }
        }
        if (const auto seal = sm.seal_candidate()) {
            (void)propose_bytes(g, encode_seal(*seal));
            count(&ledger_host_stats::seals);
        }
        for (const auto& job : sm.exports()) {
            if (job.status != "in_progress") {
                continue;
            }
            export_outcome outcome;
            if (opts_.run_export) {
                outcome = opts_.run_export(sm, job);
            } else {
                outcome.job_id = job.job_id;
                outcome.completed = false;
                outcome.message = "this node has no export store";
            }
            (void)propose_bytes(g, encode_export_finish(outcome));
            count(&ledger_host_stats::exports_finished);
        }
    }

    auto maintenance_loop() -> void {
        std::unique_lock lock(run_mutex_);
        while (!stopping_) {
            lock.unlock();
            try {
                maintain();
            } catch (const std::exception&) {
                count(&ledger_host_stats::maintenance_errors);
            }
            lock.lock();
            run_cv_.wait_for(lock, opts_.maintenance_interval, [this] { return stopping_; });
        }
    }

    auto count(std::uint64_t ledger_host_stats::* field) -> void {
        const std::lock_guard lock(mutex_);
        ++(stats_.*field);
    }

    Host* host_;
    std::shared_ptr<typename group_sm::context> ctx_;
    options opts_;

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<hosted>> hosted_;
    std::map<group_id_type, std::uint64_t> ready_term_;  // terms caught up in
    ledger_host_stats stats_;

    std::mutex run_mutex_;
    std::condition_variable run_cv_;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace kythira::ledger
