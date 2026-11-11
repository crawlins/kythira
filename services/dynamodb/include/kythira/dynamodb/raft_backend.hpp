// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file raft_backend.hpp
/// @brief The Raft-hosted `dynamo_backend`: the catalog and the items on
///        `multi_raft` groups (`.kiro/specs/dynamodb-compatible-api/` task 7).
///
/// Every node runs a `multi_raft` host whose groups run
/// `dynamo_txn_group_state_machine` (`dynamo_group_state_machine` inside
/// core's transaction wrapper): one catalog group, whose range holds
/// `\0meta/dynamodb/`, and item groups over `t/`. `bootstrap_descriptors`
/// gives the first layout: the catalog group over everything below `t/`, and
/// one item group over `t/` and above.
///
/// `multi_raft` never forwards: a command submitted on a node that does not
/// lead the owning group fails with the leader's id as a hint. This backend
/// does the hop. Each call becomes an *op* (a tag byte and its payload, the
/// same bytes whether it runs here or on a peer); `serve(op)` runs one on
/// this node and answers ok, "not the leader, try N", or an error. A call
/// runs its op here first, then on whichever node the answers point to,
/// through the deployment's forwarder (an internal endpoint between
/// gateways, or a direct call in tests), until it gets an answer or the
/// deadline passes.
///
/// | Op | What | Where it must run |
/// |---|---|---|
/// | catalog | commit a catalog entry | the catalog leader |
/// | write | commit an item entry (routed by its base key) | the item key's leader |
/// | read | `GetItem` | the leader when consistent, else any replica |
/// | page | part of a range read inside one group | as for read |
/// | describe, list | read the catalog | the catalog leader |
/// | drop | one `drop_table_step` on a named group | that group's leader |
/// | txn | one transaction envelope on the group owning its keys | that group's leader |
///
/// Transactions (task 10) run on core's tier-1 coordinator. Its client here
/// (`txn_client`) sends each envelope as a txn op, so a prepare reaches its
/// group's leader by the same hops as a write; envelopes are idempotent by
/// transaction id, so a retried op is safe. A routing refusal (the keys no
/// longer share a group) comes back as its own answer, and the coordinator
/// re-partitions. The host's resolver reaches primaries led elsewhere the
/// same way. A plain write to an item a transaction holds is refused by the
/// wrapper and answered as `TransactionConflictException`.
///
/// A consistent read takes a read barrier on the leader (`read_state`), then
/// reads the leader's committed state. A range read walks the groups its
/// range overlaps, in its direction, one page at a time: each page is copied
/// out under the group's lock and visited outside it, so a slow visitor never
/// holds a Raft node, and a page names the part of the range it covered so
/// a split or merge between pages loses nothing.
///
/// DescribeTable and ListTables go to the catalog leader through a barrier
/// too, and so does the describe every item call starts with, so a table is
/// never served after its delete committed. A replica-local descriptor cache
/// is a later optimization.
///
/// The catalog leader also does each table's follow-on work, as
/// `local_backend` does in-process: CREATING and UPDATING become ACTIVE, and
/// a delete job runs bounded drop steps on every group the table's range
/// overlaps, recording each as a `job_progress` entry, until a full pass
/// finds nothing left. `start()` runs that on a worker thread; tests may call
/// `settle_once()` instead.

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/group_state_machine.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/item_state_machine.hpp>
#include <kythira/dynamodb/key_layout.hpp>

#include <raft/composite_node_id.hpp>
#include <raft/future_default.hpp>
#include <raft/persistent_state_machine.hpp>
#include <raft/shard_exceptions.hpp>
#include <raft/txn_coordinator.hpp>
#include <raft/txn_envelope.hpp>
#include <raft/txn_state_machine.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

/// Timeouts and sizes of a `raft_backend`.
struct raft_backend_options {
    /// One attempt's wait for a commit or a read barrier.
    std::chrono::milliseconds attempt_timeout{5000};
    /// A whole call's budget, across leader changes and hops.
    std::chrono::milliseconds deadline{15000};
    /// The pause before retrying when no node knows the leader.
    std::chrono::milliseconds retry_backoff{20};
    /// How often the catalog leader looks for follow-on work.
    std::chrono::milliseconds settle_interval{200};
    /// The most item bytes one range-read page carries.
    std::size_t page_bytes = std::size_t{1} << 20U;
    /// Hops one call may take before it starts over from this node.
    std::size_t max_hops = 4;
    /// Items deleted per drop step.
    std::uint32_t drop_step_keys = 1000;
    /// Each transaction's lock TTL and budget (read committed: DynamoDB's
    /// transactions lock every item they touch).
    kythira::txn_options txn{};
};

/// A transaction op's keys no longer share one group; the coordinator
/// re-partitions and sends again.
class txn_stale_route : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace raft_backend_detail {

/// The op tags.
enum class op_tag : std::uint8_t {
    catalog = 1,
    write = 2,
    read = 3,
    page = 4,
    describe = 5,
    list = 6,
    drop = 7,
    txn = 8,
};

/// The answer tags.
enum class answer_tag : std::uint8_t {
    ok = 0,
    /// Not served here: retry on the hinted node, or after a pause.
    not_leader = 1,
    /// Failed; the text says why.
    error = 2,
    /// A txn op's keys span groups now (a split or merge): re-partition.
    stale_route = 3,
};

inline void put_range(std::string& out, const kythira::key_range& r) {
    detail::put_bytes(out, r.start);
    out.push_back(r.end ? '\1' : '\0');
    if (r.end) {
        detail::put_bytes(out, *r.end);
    }
}

[[nodiscard]] inline auto get_range(detail::reader& in) -> std::optional<kythira::key_range> {
    auto start = in.bytes();
    const auto has_end = in.byte();
    if (!start || !has_end || *has_end > 1) {
        return std::nullopt;
    }
    kythira::key_range r{.start = std::move(*start), .end = std::nullopt};
    if (*has_end == 1) {
        auto end = in.bytes();
        if (!end) {
            return std::nullopt;
        }
        r.end = std::move(*end);
    }
    return r;
}

/// `a ∩ b`.
[[nodiscard]] inline auto intersect(const kythira::key_range& a, const kythira::key_range& b)
    -> kythira::key_range {
    kythira::key_range out{.start = std::max(a.start, b.start), .end = a.end};
    if (b.end && (!out.end || *b.end < *out.end)) {
        out.end = b.end;
    }
    return out;
}

/// One page of a range read.
struct page {
    std::vector<std::pair<std::string, item>> items;
    /// More items remain in the group's part of the range after the last.
    bool more = false;
    /// The part of the asked range this group covers (it is all done when
    /// `more` is false).
    kythira::key_range covered;
};

[[nodiscard]] inline auto encode_page(const page& p) -> std::string {
    std::string out;
    detail::put_varint(out, p.items.size());
    for (const auto& [key, it] : p.items) {
        detail::put_bytes(out, key);
        detail::put_bytes(out, encode_item(it));
    }
    out.push_back(p.more ? '\1' : '\0');
    put_range(out, p.covered);
    return out;
}

[[nodiscard]] inline auto decode_page(std::string_view bytes) -> std::optional<page> {
    detail::reader in(bytes);
    const auto n = in.count();
    if (!n) {
        return std::nullopt;
    }
    page p;
    for (std::size_t i = 0; i < *n; ++i) {
        auto key = in.bytes();
        auto raw = in.bytes();
        if (!key || !raw) {
            return std::nullopt;
        }
        auto it = decode_item(*raw);
        if (!it) {
            return std::nullopt;
        }
        p.items.emplace_back(std::move(*key), std::move(*it));
    }
    const auto more = in.byte();
    auto covered = get_range(in);
    if (!more || *more > 1 || !covered || !in.done()) {
        return std::nullopt;
    }
    p.more = *more == 1;
    p.covered = std::move(*covered);
    return p;
}

[[nodiscard]] inline auto to_bytes(std::string_view s) -> std::vector<std::byte> {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

[[nodiscard]] inline auto view(const std::vector<std::byte>& b) -> std::string_view {
    return {reinterpret_cast<const char*>(b.data()), b.size()};  // NOLINT
}

}  // namespace raft_backend_detail

/// The backend over a `multi_raft` host whose state machine is a
/// `dynamo_group_state_machine`.
template<typename Host> class raft_backend {
    static_assert(kythira::is_transactional_state_machine_v<typename Host::state_machine_type>,
                  "raft_backend: the host's groups must run dynamo_txn_group_state_machine");

public:
    using host_type = Host;
    using node_id_type = typename Host::node_id_type;
    using group_id_type = typename Host::group_id_type;
    using descriptor_type = typename Host::descriptor_type;
    using clock_fn = std::function<std::uint64_t()>;
    /// Sends an op to a peer and returns its answer, or nothing when the
    /// peer cannot be reached. Blocking; must not throw.
    using forwarder =
        std::function<std::optional<std::string>(const node_id_type&, const std::string& op)>;

    raft_backend(Host& host, forwarder forward, raft_backend_options options = {},
                 clock_fn clock = local_backend<>::system_clock_ms())
        : _host(host),
          _forward(std::move(forward)),
          _options(options),
          _clock(std::move(clock)),
          _txn(*this) {
        // The resolver's way to a primary this node does not lead.
        _host.set_txn_forwarder(
            [this](const std::vector<std::string>& keys, const std::vector<std::byte>& envelope,
                   std::chrono::milliseconds) { return send_txn(keys, envelope); });
    }

    ~raft_backend() {
        _host.set_txn_forwarder({});
        stop();
    }

    raft_backend(const raft_backend&) = delete;
    auto operator=(const raft_backend&) -> raft_backend& = delete;
    raft_backend(raft_backend&&) = delete;
    auto operator=(raft_backend&&) -> raft_backend& = delete;

    /// The first layout: the catalog group (`catalog_group`) over everything
    /// below `t/`, and one item group (`item_group`) over `t/` and above.
    /// Every node creates both with the same voters.
    [[nodiscard]] static auto bootstrap_descriptors(const std::vector<node_id_type>& voters,
                                                    group_id_type catalog_group = 1,
                                                    group_id_type item_group = 2)
        -> std::vector<descriptor_type> {
        descriptor_type catalog{};
        catalog._group_id = catalog_group;
        catalog._range._start = std::nullopt;
        catalog._range._end = std::string("t/");
        catalog._voters = voters;
        descriptor_type items{};
        items._group_id = item_group;
        items._range._start = std::string("t/");
        items._range._end = std::nullopt;
        items._voters = voters;
        return {catalog, items};
    }

    // -- dynamo_backend -------------------------------------------------------

    auto catalog(const catalog_command& c) -> catalog_result {
        std::string op(1, static_cast<char>(raft_backend_detail::op_tag::catalog));
        op += encode_catalog_command(c);
        const auto out = run(op, std::nullopt);
        auto r = decode_catalog_result(out);
        if (!r) {
            throw std::runtime_error("raft_backend: unreadable catalog result");
        }
        return *r;
    }

    auto write(const item_command& c) -> item_result {
        std::string op(1, static_cast<char>(raft_backend_detail::op_tag::write));
        op += encode_item_command(c);
        return item_answer(run(op, std::nullopt));
    }

    auto read(std::uint64_t table_id, const key_schema& schema, const item& key, bool consistent)
        -> std::optional<item> {
        if (!base_item_key(table_id, schema, key)) {
            return std::nullopt;
        }
        item_command c;
        c.table_id = table_id;
        c.schema = schema;
        c.payload = key;
        std::string op(1, static_cast<char>(raft_backend_detail::op_tag::read));
        op.push_back(consistent ? '\1' : '\0');
        op += encode_item_command(c);
        const auto out = run(op, std::nullopt);
        if (out.empty()) {
            return std::nullopt;
        }
        auto it = decode_item(out);
        if (!it) {
            throw std::runtime_error("raft_backend: unreadable item");
        }
        return it;
    }

    auto read_range(const kythira::key_range& range, kythira::scan_direction direction,
                    bool consistent, const range_visitor& visit) -> void {
        using raft_backend_detail::op_tag;
        const bool forward = direction == kythira::scan_direction::forward;
        auto remaining = range;
        const auto deadline = std::chrono::steady_clock::now() + _options.deadline;
        while (!remaining.empty()) {
            const auto desc = edge_group(remaining, forward);
            if (!desc) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error("raft_backend: no group owns part of the range");
                }
                std::this_thread::sleep_for(_options.retry_backoff);
                continue;
            }
            std::string op(1, static_cast<char>(op_tag::page));
            detail::put_varint(op, static_cast<std::uint64_t>(desc->_group_id));
            raft_backend_detail::put_range(op, remaining);
            op.push_back(forward ? '\1' : '\0');
            op.push_back(consistent ? '\1' : '\0');
            const auto p = raft_backend_detail::decode_page(run(op, desc));
            if (!p) {
                throw std::runtime_error("raft_backend: unreadable page");
            }
            for (const auto& [key, it] : p->items) {
                if (!visit(key, it)) {
                    return;
                }
            }
            if (p->more && !p->items.empty()) {
                const auto& last = p->items.back().first;
                if (forward) {
                    remaining.start = last + '\0';
                } else {
                    remaining.end = last;
                }
                continue;
            }
            // This group's part is done; go past it.
            if (forward) {
                if (!p->covered.end) {
                    return;
                }
                remaining.start = *p->covered.end;
            } else {
                if (p->covered.start <= remaining.start) {
                    return;
                }
                remaining.end = p->covered.start;
            }
        }
    }

    auto describe(std::string_view name) -> std::optional<table_descriptor> {
        std::string op(1, static_cast<char>(raft_backend_detail::op_tag::describe));
        detail::put_bytes(op, name);
        const auto out = run(op, std::nullopt);
        if (out.empty()) {
            return std::nullopt;
        }
        auto d = decode_table_descriptor(out);
        if (!d) {
            throw std::runtime_error("raft_backend: unreadable table descriptor");
        }
        return d;
    }

    auto list_tables(std::string_view exclusive_start, std::size_t limit)
        -> std::vector<std::string> {
        std::string op(1, static_cast<char>(raft_backend_detail::op_tag::list));
        detail::put_bytes(op, exclusive_start);
        detail::put_varint(op, limit);
        const auto out = run(op, std::nullopt);
        detail::reader in(out);
        const auto n = in.count();
        if (!n) {
            throw std::runtime_error("raft_backend: unreadable table list");
        }
        std::vector<std::string> names;
        for (std::size_t i = 0; i < *n; ++i) {
            auto s = in.bytes();
            if (!s) {
                throw std::runtime_error("raft_backend: unreadable table list");
            }
            names.push_back(std::move(*s));
        }
        return names;
    }

    auto now_ms() -> std::uint64_t { return _clock(); }

    auto transact(const std::vector<item_command>& actions) -> txn_report {
        kythira::transaction_coordinator<txn_client, std::string> coordinator(_txn, _options.txn);
        std::vector<std::string> keys;
        keys.reserve(actions.size());
        coordinator.begin();
        for (const auto& a : actions) {
            auto key = base_item_key(a.table_id, a.schema, a.payload);
            if (!key) {
                throw std::invalid_argument("raft_backend: a transaction action without a key");
            }
            coordinator.put(*key, raft_backend_detail::to_bytes(encode_item_command(a)));
            keys.push_back(std::move(*key));
        }
        const auto out = coordinator.commit();
        txn_report r;
        r.results.resize(actions.size());
        r.conflicts.assign(actions.size(), false);
        const auto index_of = [&](const std::string& key) -> std::optional<std::size_t> {
            const auto it = std::find(keys.begin(), keys.end(), key);
            if (it == keys.end()) {
                return std::nullopt;
            }
            return static_cast<std::size_t>(it - keys.begin());
        };
        for (const auto& v : out._validations) {
            const auto i = index_of(v._key);
            if (!i || v._detail.empty()) {
                continue;
            }
            auto decoded = decode_item_result(raft_backend_detail::view(v._detail));
            if (!decoded) {
                throw std::runtime_error("raft_backend: unreadable validation detail");
            }
            r.results[*i] = std::move(*decoded);
        }
        switch (out._kind) {
            case kythira::txn_outcome_kind::committed:
                for (std::size_t i = 0; i < actions.size(); ++i) {
                    // A one-phase commit retried after a timeout is answered
                    // from memory, without the reads: never pass that off as
                    // items that are not there.
                    if (actions[i].op == item_command::kind::read_lock && !r.results[i]) {
                        throw std::runtime_error(
                            "raft_backend: a transaction read lost its result");
                    }
                }
                return r;
            case kythira::txn_outcome_kind::unknown:
                throw std::runtime_error("raft_backend: transaction outcome unknown: " +
                                         out._detail);
            case kythira::txn_outcome_kind::aborted:
                break;
        }
        switch (out._reason) {
            case kythira::txn_abort_reason::validation_failed:
                r.outcome = txn_report::status::cancelled;
                return r;
            case kythira::txn_abort_reason::write_conflict:
                r.outcome = txn_report::status::cancelled;
                if (out._conflict_key) {
                    if (const auto i = index_of(*out._conflict_key)) {
                        r.conflicts[*i] = true;
                    }
                }
                return r;
            default:
                throw std::runtime_error("raft_backend: transaction aborted (" +
                                         std::to_string(static_cast<int>(out._reason)) + ") " +
                                         out._detail);
        }
    }

    // -- Serving ops ----------------------------------------------------------

    /// Runs one op on this node and returns its answer. The forward endpoint
    /// calls this with the bytes a peer sent; it never forwards again.
    auto serve(std::string_view op) -> std::string {
        using raft_backend_detail::op_tag;
        if (op.empty()) {
            return error_answer("empty op");
        }
        const auto tag = static_cast<op_tag>(op[0]);
        const auto body = op.substr(1);
        try {
            switch (tag) {
                case op_tag::catalog:
                    return serve_catalog(body);
                case op_tag::write:
                    return serve_write(body);
                case op_tag::read:
                    return serve_read(body);
                case op_tag::page:
                    return serve_page(body);
                case op_tag::describe:
                case op_tag::list:
                    return serve_catalog_read(tag, body);
                case op_tag::drop:
                    return serve_drop(body);
                case op_tag::txn:
                    return serve_txn(body);
            }
            return error_answer("unknown op");
        } catch (const kythira::shard_not_leader_exception<group_id_type, node_id_type>& e) {
            return not_leader_answer(e.leader_hint());
        } catch (const kythira::shard_exception&) {
            // Routing is moving (a split, a merge, a stale row): retry.
            return not_leader_answer(std::nullopt);
        } catch (const std::exception& e) {
            // A write that failed after it was proposed may still commit, so
            // it is not retried here; the client sees a 500 and its SDK
            // retries, as against AWS. Reads and follow-on work are safe to
            // repeat.
            if (tag == op_tag::write || tag == op_tag::catalog) {
                return error_answer(e.what());
            }
            return not_leader_answer(std::nullopt);
        }
    }

    // -- Follow-on work -------------------------------------------------------

    /// Starts the worker that does the catalog leader's follow-on work.
    auto start() -> void {
        const std::lock_guard lock(_worker_mu);
        if (_worker.joinable()) {
            return;
        }
        _stopping = false;
        _worker = std::thread([this] { worker_loop(); });
    }

    auto stop() -> void {
        {
            const std::lock_guard lock(_worker_mu);
            _stopping = true;
        }
        _worker_cv.notify_all();
        if (_worker.joinable()) {
            _worker.join();
        }
    }

    /// One pass of follow-on work, when this node leads the catalog group.
    /// Returns whether it did anything.
    auto settle_once() -> bool {
        const auto catalog_desc = _host.resolve(std::string(k_catalog_prefix));
        if (!catalog_desc) {
            return false;
        }
        auto* n = _host.group_node(catalog_desc->_group_id);
        if (n == nullptr || !n->is_leader()) {
            return false;
        }
        struct snapshot {
            std::vector<table_descriptor> pending;
            std::vector<delete_job> jobs;
        };
        const auto s = _host.with_group_state_machine(
            catalog_desc->_group_id, [](auto& sm, const descriptor_type&) {
                snapshot out;
                const auto* cat = sm.inner().catalog();
                if (cat == nullptr) {
                    return out;
                }
                for (const auto& name : cat->list_tables("", SIZE_MAX)) {
                    const auto d = cat->describe(name);
                    if (d && (d->status == table_status::creating ||
                              d->status == table_status::updating)) {
                        out.pending.push_back(*d);
                    }
                }
                out.jobs = cat->delete_jobs();
                return out;
            });
        if (!s) {
            return false;
        }
        bool did = false;
        for (const auto& d : s->pending) {
            catalog_command c;
            c.op = catalog_command::kind::set_status;
            c.table.name = d.name;
            c.table.table_id = d.table_id;
            c.from = d.status;
            c.to = table_status::active;
            (void)catalog(c);
            did = true;
        }
        for (const auto& job : s->jobs) {
            run_delete_job(job);
            did = true;
        }
        return did;
    }

private:
    // -- Transactions ---------------------------------------------------------

    /// The coordinator's way to the groups: every envelope is a txn op.
    class txn_client {
    public:
        using group_id_type = typename Host::group_id_type;
        using send_future = kythira::future_default<std::vector<std::byte>>;

        explicit txn_client(raft_backend& backend) : _backend(backend) {}
        txn_client(const txn_client&) = delete;
        auto operator=(const txn_client&) -> txn_client& = delete;
        txn_client(txn_client&&) = delete;
        auto operator=(txn_client&&) -> txn_client& = delete;

        ~txn_client() {
            const std::lock_guard lock(_mu);
            for (auto& s : _senders) {
                s.thread.join();
            }
        }

        [[nodiscard]] auto resolve(const std::string& key) -> std::optional<group_id_type> {
            const auto desc = _backend._host.resolve(key);
            if (!desc) {
                return std::nullopt;
            }
            return desc->_group_id;
        }

        /// Each send runs on a thread of its own, so the coordinator's
        /// prepares to several groups are in flight together.
        [[nodiscard]] auto send(const std::vector<std::string>& keys,
                                const std::vector<std::byte>& envelope, std::chrono::milliseconds)
            -> send_future {
            kythira::promise_default<std::vector<std::byte>> promise;
            auto result = promise.getFuture();
            const std::lock_guard lock(_mu);
            reap();
            auto& s = _senders.emplace_back();
            s.done = std::make_shared<std::atomic<bool>>(false);
            s.thread = std::thread(
                [this, keys, envelope, promise = std::move(promise), done = s.done]() mutable {
                    try {
                        promise.setValue(_backend.send_txn(keys, envelope));
                    } catch (...) {
                        promise.setException(std::current_exception());
                    }
                    done->store(true);
                });
            return result;
        }

        [[nodiscard]] auto refresh(std::exception_ptr error) -> bool {
            try {
                std::rethrow_exception(std::move(error));
            } catch (const txn_stale_route&) {
                // This node's routing map catches up as its replicas apply
                // the split or merge.
                std::this_thread::sleep_for(_backend._options.retry_backoff);
                return true;
            } catch (...) {
                return false;
            }
        }

        /// Transactions here read through `validate` (a `read_lock`), never
        /// through the coordinator's `get`.
        [[nodiscard]] auto read(const std::string&, std::chrono::milliseconds)
            -> kythira::txn_read_result<std::string> {
            throw std::logic_error("raft_backend: transaction reads go through read_lock");
        }

        [[nodiscard]] auto now_ms() const -> std::uint64_t { return _backend._host.txn_now_ms(); }

    private:
        struct sender {
            std::thread thread;
            std::shared_ptr<std::atomic<bool>> done;
        };

        auto reap() -> void {
            for (auto it = _senders.begin(); it != _senders.end();) {
                if (it->done->load()) {
                    it->thread.join();
                    it = _senders.erase(it);
                } else {
                    ++it;
                }
            }
        }

        raft_backend& _backend;
        std::mutex _mu;
        std::list<sender> _senders;
    };
    static_assert(kythira::txn_participant_client<txn_client, std::string>);

    /// Runs one envelope on the group owning `keys` and returns the encoded
    /// `txn_result`. Throws `txn_stale_route` when the keys span groups now.
    auto send_txn(const std::vector<std::string>& keys, const std::vector<std::byte>& envelope)
        -> std::vector<std::byte> {
        std::string op(1, static_cast<char>(raft_backend_detail::op_tag::txn));
        detail::put_varint(op, keys.size());
        for (const auto& k : keys) {
            detail::put_bytes(op, k);
        }
        detail::put_bytes(op, raft_backend_detail::view(envelope));
        return raft_backend_detail::to_bytes(run(op, std::nullopt));
    }

    // -- Calls ----------------------------------------------------------------

    /// Runs `op` here, then wherever the answers point, until it is served.
    /// `desc` names the group for an op that targets one (a page), so a node
    /// with no replica of it starts at the group's hinted leader.
    auto run(const std::string& op, const std::optional<descriptor_type>& desc) -> std::string {
        using raft_backend_detail::answer_tag;
        const auto deadline = std::chrono::steady_clock::now() + _options.deadline;
        std::optional<node_id_type> target;
        if (desc && _host.group_node(desc->_group_id) == nullptr) {
            target = desc->_leader_hint      ? desc->_leader_hint
                     : desc->_voters.empty() ? std::nullopt
                                             : std::optional<node_id_type>(desc->_voters.front());
        }
        std::size_t hops = 0;
        for (;;) {
            std::optional<std::string> answer;
            if (!target || *target == _host.node_id()) {
                answer = serve(op);
            } else if (_forward) {
                answer = _forward(*target, op);
            }
            std::optional<node_id_type> hint;
            if (answer && !answer->empty()) {
                const auto tag = static_cast<answer_tag>((*answer)[0]);
                if (tag == answer_tag::ok) {
                    return answer->substr(1);
                }
                if (tag == answer_tag::error) {
                    throw std::runtime_error("raft_backend: " + answer->substr(1));
                }
                if (tag == answer_tag::stale_route) {
                    throw txn_stale_route("raft_backend: the transaction's keys moved");
                }
                hint = decode_hint(std::string_view(*answer).substr(1));
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("raft_backend: no leader answered before the deadline");
            }
            if (hint && hint != target && hops < _options.max_hops) {
                target = hint;
                ++hops;
                continue;
            }
            // Nobody knows the leader yet (an election), or the hints go in
            // circles: pause and start over from this node.
            std::this_thread::sleep_for(_options.retry_backoff);
            target.reset();
            hops = 0;
        }
    }

    [[nodiscard]] static auto item_answer(const std::string& out) -> item_result {
        auto r = decode_item_result(out);
        if (!r) {
            throw std::runtime_error("raft_backend: unreadable item result");
        }
        return *r;
    }

    /// The group at the start (forward) or end (reverse) of `r`, from this
    /// node's routing map.
    [[nodiscard]] auto edge_group(const kythira::key_range& r, bool forward) const
        -> std::optional<descriptor_type> {
        kythira::shard_range<std::string> sr;
        sr._start = r.start;
        sr._end = r.end;
        const auto groups = _host.shard_map_snapshot().range_scan(sr);
        if (groups.empty()) {
            return std::nullopt;
        }
        return forward ? groups.front() : groups.back();
    }

    // -- Serving --------------------------------------------------------------

    [[nodiscard]] static auto ok_answer(std::string_view payload) -> std::string {
        std::string out(1, static_cast<char>(raft_backend_detail::answer_tag::ok));
        out += payload;
        return out;
    }

    [[nodiscard]] static auto error_answer(std::string_view why) -> std::string {
        std::string out(1, static_cast<char>(raft_backend_detail::answer_tag::error));
        out += why;
        return out;
    }

    [[nodiscard]] static auto not_leader_answer(const std::optional<node_id_type>& hint)
        -> std::string {
        std::string out(1, static_cast<char>(raft_backend_detail::answer_tag::not_leader));
        out.push_back(hint ? '\1' : '\0');
        if (hint) {
            detail::put_bytes(out, kythira::node_id_traits<node_id_type>::to_text(*hint));
        }
        return out;
    }

    [[nodiscard]] static auto decode_hint(std::string_view body) -> std::optional<node_id_type> {
        detail::reader in(body);
        const auto has = in.byte();
        if (!has || *has != 1) {
            return std::nullopt;
        }
        const auto text = in.bytes();
        if (!text) {
            return std::nullopt;
        }
        return kythira::node_id_traits<node_id_type>::from_text(*text);
    }

    /// Waits for `f` and returns its bytes.
    template<typename Future> auto await(Future&& f) -> std::vector<std::byte> {
        if (!f.wait(_options.attempt_timeout)) {
            throw std::runtime_error("timed out");
        }
        return std::forward<Future>(f).get();
    }

    auto serve_catalog(std::string_view body) -> std::string {
        const auto out = await(_host.submit_command(std::string(k_catalog_prefix),
                                                    raft_backend_detail::to_bytes(body),
                                                    _options.attempt_timeout));
        return ok_answer(raft_backend_detail::view(out));
    }

    auto serve_write(std::string_view body) -> std::string {
        item_command c;
        if (decode_item_command(body, c) != item_command_status::ok) {
            return error_answer("unreadable item command");
        }
        const auto key = base_item_key(c.table_id, c.schema, c.payload);
        if (!key) {
            return error_answer("item command without a key");
        }
        try {
            const auto out = await(_host.submit_command(*key, raft_backend_detail::to_bytes(body),
                                                        _options.attempt_timeout));
            return ok_answer(raft_backend_detail::view(out));
        } catch (const kythira::key_locked_exception<std::string>&) {
            // Requirement 12.3: a plain write meeting a transaction's lock.
            item_result r;
            r.outcome = item_result::status::error;
            r.error_type = dynamodb_error_style().service_prefix + "TransactionConflictException";
            r.error_message = "Transaction is ongoing for the item";
            return ok_answer(encode_item_result(r));
        }
    }

    auto serve_txn(std::string_view body) -> std::string {
        detail::reader in(body);
        const auto n = in.count();
        if (!n) {
            return error_answer("unreadable txn op");
        }
        std::vector<std::string> keys;
        for (std::size_t i = 0; i < *n; ++i) {
            auto k = in.bytes();
            if (!k) {
                return error_answer("unreadable txn op");
            }
            keys.push_back(std::move(*k));
        }
        auto envelope = in.bytes();
        if (!envelope || !in.done() || keys.empty()) {
            return error_answer("unreadable txn op");
        }
        try {
            const auto out = await(_host.submit_txn(keys, raft_backend_detail::to_bytes(*envelope),
                                                    _options.attempt_timeout));
            return ok_answer(raft_backend_detail::view(out));
        } catch (const kythira::shard_epoch_mismatch_exception<group_id_type, std::string,
                                                               node_id_type>&) {
            return std::string(1, static_cast<char>(raft_backend_detail::answer_tag::stale_route));
        }
    }

    auto serve_drop(std::string_view body) -> std::string {
        detail::reader in(body);
        const auto group = in.varint();
        auto cmd = in.bytes();
        if (!group || !cmd || !in.done()) {
            return error_answer("unreadable drop op");
        }
        const auto desc = _host.shard_map_snapshot().find(static_cast<group_id_type>(*group));
        if (!desc) {
            return not_leader_answer(std::nullopt);
        }
        const auto out = await(_host.submit_command(desc->_group_id, desc->_epoch,
                                                    raft_backend_detail::to_bytes(*cmd),
                                                    _options.attempt_timeout));
        return ok_answer(raft_backend_detail::view(out));
    }

    /// The read barrier when `consistent`: this node must lead the group
    /// owning `key`, and has applied everything committed before the call.
    auto barrier(const std::string& key, bool consistent) -> void {
        if (consistent) {
            (void)await(_host.read_state(key, _options.attempt_timeout));
        }
    }

    auto serve_read(std::string_view body) -> std::string {
        if (body.empty()) {
            return error_answer("unreadable read op");
        }
        const bool consistent = body[0] == '\1';
        item_command c;
        if (decode_item_command(body.substr(1), c) != item_command_status::ok) {
            return error_answer("unreadable read op");
        }
        const auto key = base_item_key(c.table_id, c.schema, c.payload);
        if (!key) {
            return ok_answer("");
        }
        barrier(*key, consistent);
        const auto desc = _host.resolve(*key);
        if (!desc) {
            return not_leader_answer(std::nullopt);
        }
        // Empty: the replica is gone, or moved off the key since the
        // barrier (a split); either way, retry.
        const auto r = _host.with_group_state_machine(
            desc->_group_id,
            [&](auto& sm, const descriptor_type& d) -> std::optional<std::optional<item>> {
                const auto* items = sm.inner().item_group();
                if (items == nullptr || !d._range.contains(*key)) {
                    return std::nullopt;
                }
                return items->get_item(c.table_id, c.schema, c.payload);
            });
        if (!r || !*r) {
            return not_leader_answer(consistent ? std::nullopt : desc->_leader_hint);
        }
        return ok_answer(**r ? encode_item(***r) : std::string{});
    }

    auto serve_page(std::string_view body) -> std::string {
        detail::reader in(body);
        const auto group = in.varint();
        auto range = raft_backend_detail::get_range(in);
        const auto fwd = in.byte();
        const auto cons = in.byte();
        if (!group || !range || !fwd || !cons || !in.done()) {
            return error_answer("unreadable page op");
        }
        const auto g = static_cast<group_id_type>(*group);
        const bool forward = *fwd == 1;
        const bool consistent = *cons == 1;
        const auto desc = _host.shard_map_snapshot().find(g);
        if (!desc || _host.group_node(g) == nullptr) {
            return not_leader_answer(desc ? desc->_leader_hint : std::nullopt);
        }
        const auto mine = raft_backend_detail::intersect(*range, engine_range(desc->_range));
        if (mine.empty()) {
            // The local row moved; the caller re-resolves.
            return not_leader_answer(std::nullopt);
        }
        barrier(mine.start, consistent);
        const std::size_t cap = _options.page_bytes;
        const auto p = _host.with_group_state_machine(
            g, [&](auto& sm, const descriptor_type& d) -> std::optional<raft_backend_detail::page> {
                const auto* items = sm.inner().item_group();
                if (items == nullptr) {
                    return std::nullopt;
                }
                raft_backend_detail::page out;
                out.covered = raft_backend_detail::intersect(*range, engine_range(d._range));
                if (out.covered.empty()) {
                    return std::nullopt;
                }
                std::size_t bytes = 0;
                items->scan_items(
                    out.covered,
                    forward ? kythira::scan_direction::forward : kythira::scan_direction::reverse,
                    [&](std::string_view key, const item& it) {
                        if (bytes >= cap) {
                            out.more = true;
                            return false;
                        }
                        bytes += key.size() + item_size(it);
                        out.items.emplace_back(std::string(key), it);
                        return true;
                    });
                return out;
            });
        if (!p || !*p) {
            return not_leader_answer(std::nullopt);
        }
        return ok_answer(raft_backend_detail::encode_page(**p));
    }

    auto serve_catalog_read(raft_backend_detail::op_tag tag, std::string_view body) -> std::string {
        const std::string key(k_catalog_prefix);
        barrier(key, true);
        const auto desc = _host.resolve(key);
        if (!desc) {
            return not_leader_answer(std::nullopt);
        }
        detail::reader in(body);
        auto text = in.bytes();
        std::optional<std::uint64_t> limit;
        if (tag == raft_backend_detail::op_tag::list) {
            limit = in.varint();
        }
        if (!text || (tag == raft_backend_detail::op_tag::list && !limit) || !in.done()) {
            return error_answer("unreadable catalog read");
        }
        const auto out = _host.with_group_state_machine(
            desc->_group_id, [&](auto& sm, const descriptor_type&) -> std::optional<std::string> {
                const auto* cat = sm.inner().catalog();
                if (cat == nullptr) {
                    return std::nullopt;
                }
                if (tag == raft_backend_detail::op_tag::describe) {
                    const auto d = cat->describe(*text);
                    return d ? encode_table_descriptor(*d) : std::string{};
                }
                const auto names = cat->list_tables(
                    *text, static_cast<std::size_t>(std::min<std::uint64_t>(*limit, SIZE_MAX)));
                std::string list;
                detail::put_varint(list, names.size());
                for (const auto& n : names) {
                    detail::put_bytes(list, n);
                }
                return list;
            });
        if (!out || !*out) {
            return not_leader_answer(std::nullopt);
        }
        return ok_answer(**out);
    }

    [[nodiscard]] static auto engine_range(const kythira::shard_range<std::string>& r)
        -> kythira::key_range {
        return kythira::key_range{.start = r._start.value_or(std::string{}), .end = r._end};
    }

    // -- Follow-on work -------------------------------------------------------

    /// Drops a deleted table's items from every group its range overlaps,
    /// then records the job done. A pass that saw the routing map change
    /// (a split or merge under it) runs again.
    auto run_delete_job(const delete_job& job) -> void {
        const auto table = table_range(job.table_id);
        kythira::shard_range<std::string> sr;
        sr._start = table.start;
        sr._end = table.end;
        for (;;) {
            const auto before = _host.shard_map_snapshot().range_scan(sr);
            for (const auto& desc : before) {
                for (;;) {
                    std::string op(1, static_cast<char>(raft_backend_detail::op_tag::drop));
                    detail::put_varint(op, static_cast<std::uint64_t>(desc._group_id));
                    detail::put_bytes(
                        op, encode_item_command(drop_step_for(job, _options.drop_step_keys)));
                    const auto step = item_answer(run(op, desc));
                    if (step.deleted > 0) {
                        catalog_command c;
                        c.op = catalog_command::kind::job_progress;
                        c.table.table_id = job.table_id;
                        c.deleted = step.deleted;
                        (void)catalog(c);
                    }
                    if (!step.more) {
                        break;
                    }
                }
            }
            const auto after = _host.shard_map_snapshot().range_scan(sr);
            if (same_groups(before, after)) {
                break;
            }
        }
        catalog_command c;
        c.op = catalog_command::kind::job_progress;
        c.table.table_id = job.table_id;
        c.done = true;
        (void)catalog(c);
    }

    [[nodiscard]] static auto same_groups(const std::vector<descriptor_type>& a,
                                          const std::vector<descriptor_type>& b) -> bool {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                          [](const descriptor_type& x, const descriptor_type& y) {
                              return x._group_id == y._group_id && x._epoch == y._epoch;
                          });
    }

    auto worker_loop() -> void {
        std::unique_lock lock(_worker_mu);
        while (!_stopping) {
            lock.unlock();
            try {
                (void)settle_once();
            } catch (const std::exception&) {
                // Leadership moved or a peer is unreachable; the next pass
                // (here or on the new leader) resumes from the catalog.
            }
            lock.lock();
            _worker_cv.wait_for(lock, _options.settle_interval, [this] { return _stopping; });
        }
    }

    Host& _host;
    forwarder _forward;
    raft_backend_options _options;
    clock_fn _clock;

    std::mutex _worker_mu;
    std::condition_variable _worker_cv;
    bool _stopping = false;
    std::thread _worker;
    /// Last, so its send threads are joined before anything they use goes.
    txn_client _txn;
};

}  // namespace kythira::dynamodb
