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
/// | segment | a change log segment's bytes (task 15) | any node holding it |
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
#include <kythira/dynamodb/gsi_propagator.hpp>
#include <kythira/dynamodb/group_state_machine.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/item_state_machine.hpp>
#include <kythira/dynamodb/key_layout.hpp>

#include <raft/composite_node_id.hpp>
#include <raft/future_default.hpp>
#include <raft/group_storage.hpp>
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
#include <map>
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
    /// How often an item group's leader looks for GSI changes to carry when
    /// the last pass found none, and how many it takes from one group per
    /// pass (task 11).
    std::chrono::milliseconds gsi_interval{20};
    std::size_t gsi_batch = 64;
    /// How long a table looked up by id stays cached for the propagator.
    std::chrono::milliseconds table_cache_ttl{1000};
    /// How long a new GSI waits in CREATING before its backfill starts:
    /// longer than a write can be in flight, so no write that read the
    /// table before the index existed can land after the backfill passed
    /// its item. The default is the call deadline.
    std::chrono::milliseconds gsi_backfill_delay{15000};
    /// Items read and marked per backfill round.
    std::size_t gsi_backfill_batch = 256;
    /// Each transaction's lock TTL and budget (read committed: DynamoDB's
    /// transactions lock every item they touch).
    kythira::txn_options txn{};
    /// This node's part of every capacity limit (task 12, `capacity.hpp`):
    /// 1 enforces each limit whole on every node; `dynamodb_gateway_node`
    /// gives each of N voters 1/N.
    double capacity_share = 1.0;
    /// How often each item group's leader checks its change log for
    /// records past retention (task 16).
    std::chrono::milliseconds change_log_trim_interval{60000};
};

/// A transaction op's keys no longer share one group, or a page op's group
/// no longer holds the range it was sent for; the caller re-resolves and
/// sends again.
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
    /// An item's pending GSI change, read with the item (task 11).
    pending = 9,
    /// The bytes of a change log segment this node holds (task 15), for a
    /// peer that installed a snapshot naming it.
    segment = 10,
    /// An item group's change log streams, as DynamoDB Streams shards
    /// (task 16).
    shards = 11,
    /// A page of one change log stream's records (task 16).
    changes = 12,
    /// Split a group at a table's boundary (task 27.6).
    isolate = 13,
    /// Remove a group's voters outside a kept set (task 27.6).
    shrink = 14,
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
    /// Each item's stamp, when the op asked for them (task 22).
    bool stamped = false;
    std::vector<std::optional<replica_stamp>> stamps;
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
    // Stamps follow only when asked for, so a page without them reads as
    // it always did.
    if (p.stamped) {
        for (std::size_t i = 0; i < p.items.size(); ++i) {
            const auto& s = i < p.stamps.size() ? p.stamps[i] : std::nullopt;
            out.push_back(s ? '\1' : '\0');
            if (s) {
                detail::put_varint(out, s->written_ms);
                detail::put_bytes(out, s->region);
            }
        }
    }
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
    if (!more || *more > 1 || !covered) {
        return std::nullopt;
    }
    p.more = *more == 1;
    p.covered = std::move(*covered);
    if (!in.done()) {
        p.stamped = true;
        for (std::size_t i = 0; i < p.items.size(); ++i) {
            const auto has = in.byte();
            if (!has || *has > 1) {
                return std::nullopt;
            }
            if (*has == 0) {
                p.stamps.emplace_back();
                continue;
            }
            const auto ms = in.varint();
            auto region = in.bytes();
            if (!ms || !region) {
                return std::nullopt;
            }
            p.stamps.emplace_back(replica_stamp{*ms, std::move(*region)});
        }
        if (!in.done()) {
            return std::nullopt;
        }
    }
    return p;
}

/// The smallest key after `key`.
[[nodiscard]] inline auto just_after(std::string key) -> std::string {
    key.push_back('\0');
    return key;
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
          _throttler(options.capacity_share),
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

    /// The strong host's first layout (task 27): the catalog group (the
    /// strong catalog) as for a cluster, then one item group from `t/` to
    /// `strong_key_start()`, which never holds an item, and one from there
    /// up, which holds every strong table. Its change log streams start at
    /// the strong range, so a stream key says which host it belongs to.
    [[nodiscard]] static auto strong_bootstrap_descriptors(const std::vector<node_id_type>& voters)
        -> std::vector<descriptor_type> {
        auto out = bootstrap_descriptors(voters);
        out.back()._range._end = strong_key_start();
        descriptor_type strong{};
        strong._group_id = 3;
        strong._range._start = strong_key_start();
        strong._range._end = std::nullopt;
        strong._voters = voters;
        out.push_back(std::move(strong));
        return out;
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
        op += encode_item_command(stamp_written(c, _clock()));
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

    /// Carries the item's pending GSI change from this node before
    /// returning, for tables with synchronous GSIs (`gsi-consistency=sync`):
    /// a read of an index after the write sees it. The item group's
    /// propagator may carry the same change at the same time, which the
    /// entry versions make harmless.
    auto settle_item(const table_descriptor& table, const item& key) -> void {
        item_command c;
        c.table_id = table.table_id;
        c.schema = table.schema;
        c.payload = key;
        std::string op(1, static_cast<char>(raft_backend_detail::op_tag::pending));
        op += encode_item_command(c);
        const auto out = run(op, std::nullopt);
        if (out.empty()) {
            return;
        }
        detail::reader in(out);
        auto pkey = in.bytes();
        auto record = in.bytes();
        const auto has_item = in.byte();
        auto pending = record ? decode_gsi_pending(*record) : std::nullopt;
        if (!pkey || !pending || !has_item) {
            throw std::runtime_error("raft_backend: unreadable pending change");
        }
        gsi_work w;
        w.pending_key = std::move(*pkey);
        w.pending = std::move(*pending);
        if (*has_item == 1) {
            const auto bytes = in.bytes();
            w.current = bytes ? decode_item(*bytes) : std::nullopt;
            if (!w.current) {
                throw std::runtime_error("raft_backend: unreadable pending change");
            }
        }
        const auto plan = plan_propagation(table, w);
        for (const auto& write_cmd : plan.index_writes) {
            (void)write(write_cmd);
        }
        (void)write(plan.ack);
    }

    auto read_range(const kythira::key_range& range, kythira::scan_direction direction,
                    bool consistent, const range_visitor& visit) -> void {
        read_pages(range, direction, consistent, false,
                   [&](std::string_view key, const item& it, const std::optional<replica_stamp>&) {
                       return visit(key, it);
                   });
    }

    /// `read_range` (consistent, forward) with each item's stamp (task 22).
    auto read_stamped_range(const kythira::key_range& range, const stamped_visitor& visit) -> void {
        read_pages(range, kythira::scan_direction::forward, true, true, visit);
    }

private:
    void read_pages(const kythira::key_range& range, kythira::scan_direction direction,
                    bool consistent, bool stamped, const stamped_visitor& visit) {
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
            if (stamped) {
                op.push_back('\1');
            }
            std::string answer;
            try {
                answer = run(op, desc);
            } catch (const txn_stale_route&) {
                // A split or merge moved the range off the group resolved
                // above: resolve it again from the current map. Resending
                // the same op would only hear the same answer until the
                // deadline.
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error("raft_backend: the range kept moving");
                }
                std::this_thread::sleep_for(_options.retry_backoff);
                continue;
            }
            const auto p = raft_backend_detail::decode_page(answer);
            if (!p) {
                throw std::runtime_error("raft_backend: unreadable page");
            }
            for (std::size_t i = 0; i < p->items.size(); ++i) {
                const auto& [key, it] = p->items[i];
                const std::optional<replica_stamp> none;
                const auto& s = i < p->stamps.size() ? p->stamps[i] : none;
                if (!visit(key, it, s)) {
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

public:
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

    /// The node's change record store (task 15), whose segments this node
    /// serves to peers (the `segment` op). Set before `start()`.
    auto set_change_log(kythira::record_segment_store* store) -> void { _change_log = store; }

    /// Fetches and installs a change log segment of `group` from the
    /// group's other voters: the item state machine's fetcher, for the
    /// active segment a snapshot it installed names. Throws when no voter
    /// has it.
    auto fetch_change_segment(group_id_type group, const kythira::record_segment_descriptor& d)
        -> void {
        if (_change_log == nullptr) {
            throw kythira::record_segment_error("raft_backend: no change log store");
        }
        std::vector<std::string> peers;
        std::map<std::string, node_id_type> ids;
        if (const auto desc = _host.shard_map_snapshot().find(group)) {
            for (const auto& v : desc->_voters) {
                if (v == _host.node_id()) {
                    continue;
                }
                const auto text = kythira::node_id_traits<node_id_type>::to_text(v);
                peers.push_back(text);
                ids.emplace(text, v);
            }
        }
        const auto transport = [&](const std::string& peer, const std::string& stream,
                                   std::uint64_t first,
                                   std::uint64_t length) -> std::optional<std::vector<std::byte>> {
            std::string op(1, static_cast<char>(raft_backend_detail::op_tag::segment));
            detail::put_bytes(op, stream);
            detail::put_varint(op, first);
            detail::put_varint(op, length);
            const auto out = _forward(ids.at(peer), op);
            if (!out || out->empty() ||
                (*out)[0] != static_cast<char>(raft_backend_detail::answer_tag::ok)) {
                return std::nullopt;
            }
            std::vector<std::byte> bytes(out->size() - 1);
            std::memcpy(bytes.data(), out->data() + 1, bytes.size());
            return bytes;
        };
        if (!_change_log->fetch_segment(d, peers, transport)) {
            throw kythira::record_segment_error("raft_backend: no voter of the group has " +
                                                d._stream + " from " +
                                                std::to_string(d._first_seq));
        }
    }

    /// The change log streams of every item group overlapping `range`, as
    /// DynamoDB Streams shards (task 16), each once: the closed streams a
    /// split left are named by every child.
    auto change_shards(const kythira::key_range& range) -> std::vector<change_shard> {
        kythira::shard_range<std::string> sr;
        sr._start = range.start;
        sr._end = range.end;
        std::map<std::string, change_shard> by_key;
        for (const auto& desc : _host.shard_map_snapshot().range_scan(sr)) {
            std::string op(1, static_cast<char>(raft_backend_detail::op_tag::shards));
            detail::put_varint(op, static_cast<std::uint64_t>(desc._group_id));
            auto got = decode_change_shards(run(op, desc));
            if (!got) {
                throw std::runtime_error("raft_backend: unreadable change shards");
            }
            for (auto& s : *got) {
                auto [it, fresh] = by_key.try_emplace(s.stream.key, s);
                // Two children may have trimmed a shared parent unevenly:
                // keep the copy that still names the most.
                if (!fresh && !it->second.open && !s.open &&
                    s.stream.first_seq < it->second.stream.first_seq) {
                    it->second = std::move(s);
                }
            }
        }
        std::vector<change_shard> out;
        out.reserve(by_key.size());
        for (auto& [key, s] : by_key) {
            out.push_back(std::move(s));
        }
        return out;
    }

    /// Up to `max_records` records of change log stream `key` from
    /// `from_seq`, from the group that continues it (the one holding its
    /// range start), at that group's leader.
    auto read_changes(const std::string& key, std::uint64_t from_seq, std::size_t max_records)
        -> change_page {
        const auto parsed = parse_change_stream_key(key);
        if (!parsed) {
            return {};
        }
        const auto desc = _host.resolve(parsed->first);
        std::string op(1, static_cast<char>(raft_backend_detail::op_tag::changes));
        detail::put_bytes(op, key);
        detail::put_varint(op, from_seq);
        detail::put_varint(op, max_records);
        auto p = decode_change_page(run(op, desc));
        if (!p) {
            throw std::runtime_error("raft_backend: unreadable change page");
        }
        return std::move(*p);
    }

    /// Whether merging the groups over `a` and `b` would join a stream
    /// shard to two parents (Requirement 14.3): either range overlaps a
    /// table with a DynamoDB stream, enabled or disabled but still listed.
    [[nodiscard]] auto merge_vetoed(const kythira::key_range& a, const kythira::key_range& b)
        -> bool {
        for (const auto& name : list_tables("", SIZE_MAX)) {
            const auto d = describe(name);
            if (!d || d->streams.empty()) {
                continue;
            }
            const auto t = table_range(d->table_id);
            if (!raft_backend_detail::intersect(t, a).empty() ||
                !raft_backend_detail::intersect(t, b).empty()) {
                return true;
            }
        }
        return false;
    }

    /// Merges group `source` into `target` unless `merge_vetoed`; the
    /// DynamoDB entry point for a merge (the node runs no automatic ones).
    /// Returns whether the merge was proposed and committed.
    auto merge_groups(group_id_type source, group_id_type target) -> bool {
        const auto map = _host.shard_map_snapshot();
        const auto s = map.find(source);
        const auto t = map.find(target);
        if (!s || !t || merge_vetoed(engine_range(s->_range), engine_range(t->_range))) {
            return false;
        }
        (void)await(_host.merge_shards(source, target, _options.attempt_timeout));
        return true;
    }

    /// This node's capacity buckets, holding `capacity_share` of each limit.
    auto throttler() -> capacity_throttler& { return _throttler; }

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
            coordinator.put(*key, raft_backend_detail::to_bytes(
                                      encode_item_command(stamp_written(a, _clock()))));
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
                // commit() returns once the primary's group has committed;
                // the other groups' commits are still in flight. DynamoDB
                // promises every item is visible once TransactWriteItems
                // returns, and a consistent read serves the leader's applied
                // state without looking at intents, so wait for them here.
                // Without this a GetItem straight after the call could still
                // see the old value of an item in another group.
                coordinator.drain();
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

    // -- Membership (task 27.6) ---------------------------------------------

    /// Splits the groups holding table (or index) `id` so that its keys,
    /// `[table_prefix(id), table_prefix(id + 1))`, are a range of their
    /// own: groups wholly inside it hold nothing else. Both cuts are table
    /// prefixes, which the item groups' split veto accepts.
    auto isolate(std::uint64_t id) -> void {
        for (const auto& cut : {table_prefix(id), table_prefix(id + 1)}) {
            const auto deadline = std::chrono::steady_clock::now() + _options.deadline;
            for (;;) {
                const auto desc = _host.resolve(cut);
                if (!desc) {
                    throw std::runtime_error("raft_backend: no group owns " +
                                             ("table id " + std::to_string(id)));
                }
                if (desc->_range._start && *desc->_range._start == cut) {
                    break;
                }
                std::string op(1, static_cast<char>(raft_backend_detail::op_tag::isolate));
                detail::put_varint(op, static_cast<std::uint64_t>(desc->_group_id));
                detail::put_bytes(op, cut);
                (void)run(op, desc);
                // The split is applied where it ran; this node's routing
                // map learns it from its own replica or the next gossip.
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error("raft_backend: cannot isolate " +
                                             ("table id " + std::to_string(id)));
                }
                std::this_thread::sleep_for(_options.retry_backoff);
            }
        }
    }

    /// Leaves only `keep` as voters of every group wholly inside table (or
    /// index) `id`'s range (call `isolate` first). Each group's leader
    /// moves its leadership into `keep` if it must, then removes the other
    /// voters one configuration change at a time.
    auto shrink(std::uint64_t id, const std::vector<node_id_type>& keep) -> void {
        kythira::shard_range<std::string> sr;
        sr._start = table_prefix(id);
        sr._end = table_prefix(id + 1);
        for (const auto& desc : _host.shard_map_snapshot().range_scan(sr)) {
            if (inside(desc, sr)) {
                std::string op(1, static_cast<char>(raft_backend_detail::op_tag::shrink));
                detail::put_varint(op, static_cast<std::uint64_t>(desc._group_id));
                detail::put_varint(op, keep.size());
                for (const auto& n : keep) {
                    detail::put_bytes(op, kythira::node_id_traits<node_id_type>::to_text(n));
                }
                (void)run(op, desc);
            } else {
                throw std::runtime_error("raft_backend: " + ("table id " + std::to_string(id)) +
                                         " shares a group; isolate it first");
            }
        }
    }

    /// Whether this node holds a replica of a group inside table (or
    /// index) `id`'s range, and destroys those replicas when `destroy`.
    /// A region removed from a table's groups calls it after the shrink.
    auto drop_replicas(std::uint64_t id, bool destroy) -> std::size_t {
        kythira::shard_range<std::string> sr;
        sr._start = table_prefix(id);
        sr._end = table_prefix(id + 1);
        std::size_t n = 0;
        for (const auto& desc : _host.shard_map_snapshot().range_scan(sr)) {
            if (!inside(desc, sr) || !_host.has_group(desc._group_id)) {
                continue;
            }
            ++n;
            if (destroy) {
                (void)_host.destroy_group(desc._group_id,
                                          kythira::tombstone_reason::replica_removed);
            }
        }
        return n;
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
                case op_tag::pending:
                    return serve_pending(body);
                case op_tag::segment:
                    return serve_segment(body);
                case op_tag::shards:
                    return serve_shards(body);
                case op_tag::changes:
                    return serve_changes(body);
                case op_tag::isolate:
                    return serve_isolate(body);
                case op_tag::shrink:
                    return serve_shrink(body);
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
        _propagator = std::thread([this] { propagator_loop(); });
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
        if (_propagator.joinable()) {
            _propagator.join();
        }
    }

    /// Whether this node leads the catalog group, where the gateway runs
    /// the TTL sweeper (`ttl.hpp`).
    [[nodiscard]] auto leads_catalog() -> bool {
        const auto catalog_desc = _host.resolve(std::string(k_catalog_prefix));
        if (!catalog_desc) {
            return false;
        }
        auto* n = _host.group_node(catalog_desc->_group_id);
        return n != nullptr && n->is_leader();
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
            std::vector<table_descriptor> backfills;
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
                    // A restore's table stays CREATING until its items
                    // are loaded (task 18).
                    if (d &&
                        (d->status == table_status::creating ||
                         d->status == table_status::updating) &&
                        !d->restore.in_progress) {
                        out.pending.push_back(*d);
                    } else if (d && backfilling_index(*d) != nullptr) {
                        out.backfills.push_back(*d);
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
        for (const auto& d : s->backfills) {
            did = run_backfill(d) || did;
        }
        for (const auto& job : s->jobs) {
            run_delete_job(job);
            did = true;
        }
        return did;
    }

    /// One pass of GSI propagation over the item groups this node leads
    /// (`gsi_propagator.hpp`). Returns whether it carried anything.
    auto propagate_once() -> bool {
        kythira::shard_range<std::string> items;
        items._start = std::string("t/");
        items._end = prefix_end("t/");
        bool did = false;
        for (const auto& desc : _host.shard_map_snapshot().range_scan(items)) {
            auto* n = _host.group_node(desc._group_id);
            if (n == nullptr || !n->is_leader()) {
                continue;
            }
            auto& cursor = _gsi_cursor[static_cast<std::uint64_t>(desc._group_id)];
            const auto batch = _host.with_group_state_machine(
                desc._group_id, [&](auto& sm, const descriptor_type&) {
                    const auto* group = sm.inner().item_group();
                    return group == nullptr ? std::vector<gsi_work>{}
                                            : group->gsi_pending_batch(_options.gsi_batch, cursor);
                });
            if (!batch) {
                continue;
            }
            for (const auto& work : *batch) {
                const auto plan = plan_propagation(table_by_id(work.pending.table_id), work);
                for (const auto& w : plan.index_writes) {
                    (void)write(w);
                }
                (void)write(plan.ack);
                cursor = raft_backend_detail::just_after(work.pending_key);
                did = true;
            }
        }
        return did;
    }

    /// Items with GSI changes not yet carried, in the item groups this
    /// node leads: the propagation lag, in items.
    [[nodiscard]] auto gsi_lag() const -> std::size_t {
        kythira::shard_range<std::string> items;
        items._start = std::string("t/");
        items._end = prefix_end("t/");
        std::size_t total = 0;
        for (const auto& desc : _host.shard_map_snapshot().range_scan(items)) {
            auto* n = _host.group_node(desc._group_id);
            if (n == nullptr || !n->is_leader()) {
                continue;
            }
            total += _host
                         .with_group_state_machine(desc._group_id,
                                                   [](auto& sm, const descriptor_type&) {
                                                       const auto* g = sm.inner().item_group();
                                                       return g == nullptr ? std::size_t{0}
                                                                           : g->gsi_pending_count();
                                                   })
                         .value_or(0);
        }
        return total;
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

    [[nodiscard]] static auto stale_route_answer() -> std::string {
        return std::string(1, static_cast<char>(raft_backend_detail::answer_tag::stale_route));
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
        const auto key = command_key(c);
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

    /// The item's pending GSI change, at the item group's leader after a
    /// read barrier, as `pending key, record, [item]`; empty when none.
    /// Whether `desc`'s range lies wholly in `sr` (both ends set).
    [[nodiscard]] static auto inside(const descriptor_type& desc,
                                     const kythira::shard_range<std::string>& sr) -> bool {
        return desc._range._start && *desc._range._start >= *sr._start && desc._range._end &&
               *desc._range._end <= *sr._end;
    }

    /// Where a group's leader is: what this replica knows, else the
    /// routing map's hint.
    template<typename Node>
    [[nodiscard]] static auto leader_of(Node* n, const std::optional<descriptor_type>& desc)
        -> std::optional<node_id_type> {
        if (n != nullptr) {
            if (auto known = n->known_leader()) {
                return known;
            }
        }
        return desc ? desc->_leader_hint : std::nullopt;
    }

    auto serve_isolate(std::string_view body) -> std::string {
        detail::reader in(body);
        const auto group = in.varint();
        auto cut = in.bytes();
        if (!group || !cut || !in.done()) {
            return error_answer("unreadable isolate op");
        }
        const auto g = static_cast<group_id_type>(*group);
        const auto desc = _host.shard_map_snapshot().find(g);
        if (desc && desc->_range._start && *desc->_range._start == *cut) {
            return ok_answer({});
        }
        auto* n = _host.group_node(g);
        if (n == nullptr || !n->is_leader()) {
            return not_leader_answer(leader_of(n, desc));
        }
        try {
            (void)await(_host.split_shard(g, {std::string(*cut)}, _options.attempt_timeout));
        } catch (const kythira::shard_not_leader_exception<group_id_type, node_id_type>& e) {
            return not_leader_answer(e.leader_hint());
        } catch (const std::exception& e) {
            // Said, not retried: the worker logs it and tries again on its
            // next pass.
            return error_answer(std::string("cannot split for isolation: ") + e.what());
        }
        return ok_answer({});
    }

    auto serve_shrink(std::string_view body) -> std::string {
        detail::reader in(body);
        const auto group = in.varint();
        const auto count = in.varint();
        if (!group || !count) {
            return error_answer("unreadable shrink op");
        }
        std::vector<node_id_type> keep;
        for (std::uint64_t i = 0; i < *count; ++i) {
            const auto text = in.bytes();
            if (!text) {
                return error_answer("unreadable shrink op");
            }
            auto id = kythira::node_id_traits<node_id_type>::from_text(*text);
            if (!id) {
                return error_answer("unreadable shrink op");
            }
            keep.push_back(std::move(*id));
        }
        if (!in.done() || keep.empty()) {
            return error_answer("unreadable shrink op");
        }
        const auto g = static_cast<group_id_type>(*group);
        auto* n = _host.group_node(g);
        if (n == nullptr || !n->is_leader()) {
            return not_leader_answer(leader_of(n, _host.shard_map_snapshot().find(g)));
        }
        const auto kept = [&](const node_id_type& id) {
            return std::find(keep.begin(), keep.end(), id) != keep.end();
        };
        if (!kept(_host.node_id())) {
            // Hand the group to a kept voter, which then removes this one.
            (void)await(n->transfer_leadership(keep.front(), _options.attempt_timeout));
            return not_leader_answer(keep.front());
        }
        for (;;) {
            const auto m = n->current_membership();
            if (m.joint) {
                std::this_thread::sleep_for(_options.retry_backoff);
                continue;
            }
            const auto gone = std::find_if(m.voters.begin(), m.voters.end(),
                                           [&](const node_id_type& v) { return !kept(v); });
            if (gone == m.voters.end()) {
                break;
            }
            (void)await(n->remove_server(*gone));
        }
        return ok_answer({});
    }

    auto serve_segment(std::string_view body) -> std::string {
        detail::reader in(body);
        auto stream = in.bytes();
        const auto first = in.varint();
        const auto length = in.varint();
        if (!stream || !first || !length || !in.done()) {
            return error_answer("unreadable segment op");
        }
        if (_change_log == nullptr) {
            return error_answer("no change log here");
        }
        const auto bytes = _change_log->read_segment_bytes(*stream, *first, *length);
        if (!bytes) {
            return error_answer("no such segment here");
        }
        return ok_answer({reinterpret_cast<const char*>(bytes->data()), bytes->size()});  // NOLINT
    }

    /// An item group's change log streams, at its leader after a read
    /// barrier.
    auto serve_shards(std::string_view body) -> std::string {
        detail::reader in(body);
        const auto group = in.varint();
        if (!group || !in.done()) {
            return error_answer("unreadable shards op");
        }
        const auto g = static_cast<group_id_type>(*group);
        const auto desc = _host.shard_map_snapshot().find(g);
        if (!desc || _host.group_node(g) == nullptr) {
            return not_leader_answer(desc ? desc->_leader_hint : std::nullopt);
        }
        barrier(engine_range(desc->_range).start, true);
        const auto shards = _host.with_group_state_machine(
            g, [&](auto& sm, const descriptor_type&) -> std::optional<std::vector<change_shard>> {
                const auto* items = sm.inner().item_group();
                if (items == nullptr) {
                    return std::nullopt;
                }
                return items->change_shards();
            });
        if (!shards || !*shards) {
            return not_leader_answer(std::nullopt);
        }
        return ok_answer(encode_change_shards(**shards));
    }

    /// A page of a change log stream, at the leader of the group holding
    /// its range start, after a read barrier. Segments the group still
    /// names but this node lacks are fetched from its voters first.
    auto serve_changes(std::string_view body) -> std::string {
        detail::reader in(body);
        auto key = in.bytes();
        const auto from = in.varint();
        const auto max = in.varint();
        if (!key || !from || !max || !in.done()) {
            return error_answer("unreadable changes op");
        }
        const auto parsed = parse_change_stream_key(*key);
        if (!parsed) {
            return error_answer("not a change log stream");
        }
        barrier(parsed->first, true);
        const auto desc = _host.resolve(parsed->first);
        if (!desc) {
            return not_leader_answer(std::nullopt);
        }
        const auto now = _clock();
        const auto since = now > k_stream_retention_ms ? now - k_stream_retention_ms : 0;
        const auto missing = _host.with_group_state_machine(
            desc->_group_id,
            [&](auto& sm, const descriptor_type&)
                -> std::optional<std::vector<kythira::record_segment_descriptor>> {
                const auto* items = sm.inner().item_group();
                if (items == nullptr) {
                    return std::nullopt;
                }
                return items->missing_change_segments(*key, *from, since);
            });
        if (!missing || !*missing) {
            return not_leader_answer(std::nullopt);
        }
        for (const auto& d : **missing) {
            fetch_change_segment(desc->_group_id, d);
        }
        const auto p = _host.with_group_state_machine(
            desc->_group_id, [&](auto& sm, const descriptor_type&) -> std::optional<change_page> {
                const auto* items = sm.inner().item_group();
                if (items == nullptr) {
                    return std::nullopt;
                }
                return items->change_page_of(
                    *key, *from,
                    static_cast<std::size_t>(std::min<std::uint64_t>(*max, k_max_change_page)));
            });
        if (!p || !*p) {
            return not_leader_answer(std::nullopt);
        }
        return ok_answer(encode_change_page(**p));
    }

    /// The most records one `changes` op returns.
    static constexpr std::uint64_t k_max_change_page = 10000;

    auto serve_pending(std::string_view body) -> std::string {
        item_command c;
        if (decode_item_command(body, c) != item_command_status::ok) {
            return error_answer("unreadable pending op");
        }
        const auto key = base_item_key(c.table_id, c.schema, c.payload);
        if (!key) {
            return ok_answer("");
        }
        barrier(*key, true);
        const auto desc = _host.resolve(*key);
        if (!desc) {
            return not_leader_answer(std::nullopt);
        }
        const auto r = _host.with_group_state_machine(
            desc->_group_id,
            [&](auto& sm, const descriptor_type& d) -> std::optional<std::optional<gsi_work>> {
                const auto* items = sm.inner().item_group();
                if (items == nullptr || !d._range.contains(*key)) {
                    return std::nullopt;
                }
                return items->gsi_pending_for(c.table_id, c.schema, c.payload);
            });
        if (!r || !*r) {
            return not_leader_answer(std::nullopt);
        }
        const auto& w = **r;
        if (!w) {
            return ok_answer("");
        }
        std::string out;
        detail::put_bytes(out, w->pending_key);
        detail::put_bytes(out, encode_gsi_pending(w->pending));
        out.push_back(w->current ? '\1' : '\0');
        if (w->current) {
            detail::put_bytes(out, encode_item(*w->current));
        }
        return ok_answer(out);
    }

    auto serve_page(std::string_view body) -> std::string {
        detail::reader in(body);
        const auto group = in.varint();
        auto range = raft_backend_detail::get_range(in);
        const auto fwd = in.byte();
        const auto cons = in.byte();
        if (!group || !range || !fwd || !cons) {
            return error_answer("unreadable page op");
        }
        // A trailing 1 asks for each item's stamp (task 22).
        bool stamped = false;
        if (!in.done()) {
            const auto s = in.byte();
            if (!s || *s != 1 || !in.done()) {
                return error_answer("unreadable page op");
            }
            stamped = true;
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
            // The group no longer holds the range (a split or merge): the
            // caller re-resolves rather than resend this op.
            return stale_route_answer();
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
                        if (stamped) {
                            out.stamps.push_back(items->stamp_of(std::string(key)));
                        }
                        return true;
                    });
                out.stamped = stamped;
                return out;
            });
        if (!p) {
            return not_leader_answer(std::nullopt);
        }
        if (!*p) {
            return stale_route_answer();
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

    /// Backfills `table`'s new index once it has waited out
    /// `gsi_backfill_delay` (counted from when this node, as catalog
    /// leader, first saw it): marks every item of the table, group by
    /// group, for the propagators, then makes the index ACTIVE. Returns
    /// whether it ran.
    auto run_backfill(const table_descriptor& table) -> bool {
        const auto* g = backfilling_index(table);
        const auto now = std::chrono::steady_clock::now();
        const auto seen = _backfill_seen.try_emplace(g->index_id, now).first->second;
        if (now - seen < _options.gsi_backfill_delay) {
            return false;
        }
        const auto all = table_range(table.table_id);
        kythira::key_range rest = all;
        for (;;) {
            std::vector<std::string> keys;
            read_range(rest, kythira::scan_direction::forward, true,
                       [&](std::string_view key, const item&) {
                           keys.emplace_back(key);
                           return keys.size() < _options.gsi_backfill_batch;
                       });
            if (keys.empty()) {
                break;
            }
            // One mark entry per group the keys fall in; a key a split moved
            // meanwhile is marked again on its own.
            std::map<std::uint64_t, std::vector<std::string>> by_group;
            for (const auto& k : keys) {
                const auto desc = _host.resolve(k);
                by_group[desc ? static_cast<std::uint64_t>(desc->_group_id) : 0].push_back(k);
            }
            for (auto& [group, batch] : by_group) {
                if (write(backfill_marks(table, batch)).more) {
                    for (const auto& k : batch) {
                        (void)write(backfill_marks(table, {k}));
                    }
                }
            }
            if (keys.size() < _options.gsi_backfill_batch) {
                break;
            }
            rest.start = raft_backend_detail::just_after(keys.back());
        }
        const auto current = describe(table.name);
        if (current && current->table_id == table.table_id) {
            (void)catalog(activate_index(*current, g->name));
        }
        _backfill_seen.erase(g->index_id);
        return true;
    }

    [[nodiscard]] static auto same_groups(const std::vector<descriptor_type>& a,
                                          const std::vector<descriptor_type>& b) -> bool {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                          [](const descriptor_type& x, const descriptor_type& y) {
                              return x._group_id == y._group_id && x._epoch == y._epoch;
                          });
    }

    /// The table with id `id`, from a short-lived cache in front of the
    /// catalog (a describe op per miss).
    auto table_by_id(std::uint64_t id) -> std::optional<table_descriptor> {
        const auto now = std::chrono::steady_clock::now();
        if (now - _tables_read > _options.table_cache_ttl) {
            _tables.clear();
            _tables_read = now;
        }
        if (const auto it = _tables.find(id); it != _tables.end()) {
            return it->second;
        }
        std::optional<table_descriptor> found;
        for (const auto& name : list_tables("", SIZE_MAX)) {
            auto d = describe(name);
            if (d) {
                if (d->table_id == id) {
                    found = d;
                }
                _tables[d->table_id] = std::move(d);
            }
        }
        _tables.try_emplace(id, found);
        return found;
    }

    /// Proposes a retention trim to every item group this node leads whose
    /// change log holds records past retention (task 16). Returns how many
    /// it trimmed.
    auto trim_change_logs_once() -> std::size_t {
        const auto now = _clock();
        if (now <= k_stream_retention_ms) {
            return 0;
        }
        const auto cutoff = now - k_stream_retention_ms;
        kythira::shard_range<std::string> items;
        items._start = std::string("t/");
        items._end = prefix_end("t/");
        std::size_t trimmed = 0;
        for (const auto& desc : _host.shard_map_snapshot().range_scan(items)) {
            auto* n = _host.group_node(desc._group_id);
            if (n == nullptr || !n->is_leader()) {
                continue;
            }
            const auto due = _host.with_group_state_machine(
                desc._group_id, [&](auto& sm, const descriptor_type&) {
                    const auto* group = sm.inner().item_group();
                    return group != nullptr && group->change_log_trim_due(cutoff);
                });
            if (!due || !*due) {
                continue;
            }
            item_command c;
            c.op = item_command::kind::trim_change_log;
            c.written_ms = cutoff;
            std::string op(1, static_cast<char>(raft_backend_detail::op_tag::drop));
            detail::put_varint(op, static_cast<std::uint64_t>(desc._group_id));
            detail::put_bytes(op, encode_item_command(c));
            (void)run(op, desc);
            ++trimmed;
        }
        return trimmed;
    }

    auto propagator_loop() -> void {
        std::unique_lock lock(_worker_mu);
        auto next_trim = std::chrono::steady_clock::now();
        while (!_stopping) {
            lock.unlock();
            bool did = false;
            try {
                did = propagate_once();
                if (std::chrono::steady_clock::now() >= next_trim) {
                    next_trim =
                        std::chrono::steady_clock::now() + _options.change_log_trim_interval;
                    (void)trim_change_logs_once();
                }
            } catch (const std::exception&) {
                // Leadership moved or a group is electing; the change stays
                // recorded and the next pass (here or on the new leader)
                // carries it.
            }
            lock.lock();
            if (!did) {
                _worker_cv.wait_for(lock, _options.gsi_interval, [this] { return _stopping; });
            }
        }
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
    /// The node's change record store (task 15), or none.
    kythira::record_segment_store* _change_log = nullptr;
    raft_backend_options _options;
    clock_fn _clock;
    capacity_throttler _throttler;

    std::mutex _worker_mu;
    std::condition_variable _worker_cv;
    bool _stopping = false;
    std::thread _worker;
    std::thread _propagator;
    /// The propagator's place in each item group's pending changes, so a
    /// change that keeps failing does not starve the rest.
    std::map<std::uint64_t, std::string> _gsi_cursor;
    /// Catalog leader: when each backfilling index was first seen.
    std::map<std::uint64_t, std::chrono::steady_clock::time_point> _backfill_seen;
    std::map<std::uint64_t, std::optional<table_descriptor>> _tables;
    std::chrono::steady_clock::time_point _tables_read{};
    /// Last, so its send threads are joined before anything they use goes.
    txn_client _txn;
};

}  // namespace kythira::dynamodb
