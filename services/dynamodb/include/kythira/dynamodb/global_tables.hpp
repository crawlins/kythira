// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// @brief Global tables, version 2019.11.21 (task 22, Requirement 18):
/// replicas of a table in other Kythira clusters, configured as peer
/// regions, kept in step asynchronously from the change log with AWS's
/// last-writer-wins rule.
///
/// Every write to a global table is stamped with its time and the region
/// it was made in (`item_state_machine.hpp`); a write from another replica
/// applies only when its stamp is newer than the item's, so every replica
/// settles on the same item whatever order the writes arrive in. A local
/// write is stamped past the item's stamp, so it wins everywhere as it did
/// here even when this cluster's clock is behind.
///
/// The replicator runs on the node that leads the catalog. For each
/// replica of each global table:
///
///   - CREATING: it asks the peer to create the table (`CreateReplica`),
///     takes the change log's positions, copies every item with its stamp
///     (`Apply`), and marks the replica backfilled. It then tells the
///     table's other ACTIVE replicas of the new one (`SetReplicas`), which
///     start it where they are with this cluster, so a write still on its
///     way here is sent to it from there too. Last, it sends what this
///     cluster's log holds since the positions, whatever region the
///     writes came from, and marks the replica ACTIVE.
///   - ACTIVE: it sends the writes made in this region since its cursors,
///     in each store stream's order, parents first, and moves the cursors
///     on.
///   - DELETING: it asks the peer to delete its table (`DeleteReplica`),
///     tells the other replicas, and drops the replica.
///
/// A peer that cannot be reached is tried again on the next pass; a peer
/// that refuses to create the replica leaves it CREATION_FAILED. Records
/// trimmed from the change log before they were sent are lost to the
/// replica, and the replicator says so; the log keeps 24 hours.
///
/// The peer API is `POST /kythira/replication` (`front_end.hpp`), a JSON
/// object whose `Action` is one of:
///
///   - `CreateReplica`: `TableName`, `SourceRegion`, `Descriptor` (the
///     source's encoded descriptor, base64).
///   - `DeleteReplica`: `TableName`.
///   - `SetReplicas`: `TableName`, `SourceRegion`, and `Add` or `Remove`
///     naming a region.
///   - `Apply`: `TableName`, `SourceRegion`, `Writes`: a list of
///     `{"Item": ...}` or `{"Key": ..., "Delete": true}`, each with
///     `WrittenMs` and `Region`, its stamp.
///
/// Deleted items are not copied when a replica is created: a write that a
/// deletion here beat may still land on the new replica if it reaches it
/// first.

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/change_log.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/global_index.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/item_state_machine.hpp>
#include <kythira/dynamodb/key_layout.hpp>
#include <kythira/dynamodb/kinesis_destination.hpp>
#include <kythira/dynamodb/streams.hpp>

#include <raft/aws_json_protocol.hpp>
#include <raft/sigv4_verifier.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

/// The table version `DescribeTable` reports for a global table.
inline constexpr std::string_view k_global_table_version = "2019.11.21";

/// Sends one replication call to a peer region: the request object to the
/// answer object, or the error (status 0 when the peer could not be
/// reached).
using replication_send =
    std::function<aws_json::result<boost::json::object>(const boost::json::object& request)>;

/// The peer regions this cluster replicates to.
class peer_set {
public:
    void add(std::string region, replication_send send) {
        _peers.insert_or_assign(std::move(region), std::move(send));
    }

    [[nodiscard]] auto has(std::string_view region) const -> bool {
        return _peers.find(region) != _peers.end();
    }

    [[nodiscard]] auto empty() const -> bool { return _peers.empty(); }

    [[nodiscard]] auto send(std::string_view region, const boost::json::object& request) const
        -> aws_json::result<boost::json::object> {
        const auto it = _peers.find(region);
        if (it == _peers.end()) {
            aws_json::error e;
            e.status = 0;
            e.type = "UnknownPeer";
            e.message = "No peer link for region " + std::string(region);
            return e;
        }
        return it->second(request);
    }

private:
    std::map<std::string, replication_send, std::less<>> _peers;
};

/// How this cluster reaches one peer region's replication path.
struct replication_peer_config {
    /// The peer's region: the SigV4 scope its gateway checks.
    std::string region;
    /// "host:port" of one of its gateways (a load balancer in front of
    /// them all, in production).
    std::string endpoint;
    bool tls = true;
    /// PEM bundle its certificate chains to; empty uses the system's.
    std::string ca_path;
    /// The replication principal's keys, valid in the peer's cluster.
    std::string access_key_id;
    std::string secret_access_key;
    /// With per-account tables (task 28), a replication principal per
    /// account: a request naming an `Owner` listed here is signed with
    /// that account's keys, so the peer finds the table in that account.
    /// Others use the keys above.
    std::map<std::string, std::pair<std::string, std::string>, std::less<>> account_keys;
    std::chrono::milliseconds timeout{10000};
    /// The signing clock; the system clock when empty.
    std::function<std::chrono::sys_seconds()> clock;
};

namespace global_tables_detail {

/// Whether a failed call is worth trying again: the peer was not reached,
/// failed, or was busy (a table still being created, another replica
/// change under way).
[[nodiscard]] inline auto retryable(const aws_json::error& e) -> bool {
    return e.status == 0 || e.status >= 500 || e.type == "ResourceInUseException" ||
           e.type == "LimitExceededException" || e.type == "ThrottlingException" ||
           e.type.ends_with("#ResourceInUseException") ||
           e.type.ends_with("#LimitExceededException");
}

[[nodiscard]] inline auto not_found(const aws_json::error& e) -> bool {
    return e.type == "ResourceNotFoundException" || e.type.ends_with("#ResourceNotFoundException");
}

/// One write as `Apply` carries it.
[[nodiscard]] inline auto write_json(const item& it, bool deleted, const replica_stamp& stamp)
    -> boost::json::object {
    boost::json::object w;
    w[deleted ? "Key" : "Item"] = item_to_json(it);
    if (deleted) {
        w["Delete"] = true;
    }
    w["WrittenMs"] = stamp.written_ms;
    w["Region"] = stamp.region;
    return w;
}

[[nodiscard]] inline auto replica_named(std::vector<table_replica>& list, std::string_view region)
    -> table_replica* {
    const auto it = std::find_if(list.begin(), list.end(),
                                 [&](const table_replica& r) { return r.region == region; });
    return it == list.end() ? nullptr : &*it;
}

/// A rough size of a write on the wire, for batching.
[[nodiscard]] inline auto json_size(const boost::json::object& o) -> std::size_t {
    return boost::json::serialize(o).size();
}

/// The keys a request is signed with: its `Owner`'s replication principal
/// when `account_keys` lists one (task 28), else the cluster's.
[[nodiscard]] inline auto replication_keys(const replication_peer_config& cfg,
                                           const boost::json::object& request)
    -> std::pair<std::string_view, std::string_view> {
    if (const auto* owner = request.if_contains("Owner"); owner != nullptr && owner->is_string()) {
        if (const auto it = cfg.account_keys.find(std::string_view(owner->get_string()));
            it != cfg.account_keys.end()) {
            return {it->second.first, it->second.second};
        }
    }
    return {cfg.access_key_id, cfg.secret_access_key};
}

/// The headers of a SigV4-signed POST of `body` to the peer's replication
/// path.
[[nodiscard]] inline auto signed_replication_headers(const replication_peer_config& cfg,
                                                     std::string_view access_key_id,
                                                     std::string_view secret_access_key,
                                                     std::string_view body)
    -> std::vector<std::pair<std::string, std::string>> {
    namespace sd = sigv4::detail;
    const auto now =
        cfg.clock ? cfg.clock()
                  : std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto amz_date = sd::format_amz_date(now);
    const sigv4::credential_scope scope{
        .date = amz_date.substr(0, 8), .region = cfg.region, .service = "dynamodb"};
    auto host = cfg.endpoint;
    if ((cfg.tls && host.ends_with(":443")) || (!cfg.tls && host.ends_with(":80"))) {
        host = host.substr(0, host.rfind(':'));
    }
    // Signed in sorted order.
    std::vector<std::pair<std::string, std::string>> h{
        {"content-type", "application/json"},
        {"host", host},
        {"x-amz-date", amz_date},
    };
    std::string canonical = "POST\n" + std::string(k_replication_path) + "\n\n";
    std::string names;
    for (const auto& [k, v] : h) {
        canonical += k + ":" + v + "\n";
        names += (names.empty() ? "" : ";") + k;
    }
    canonical += "\n" + names + "\n" + sd::sha256_hex(body);
    const auto to_sign = std::string(sigv4::k_algorithm) + "\n" + amz_date + "\n" + scope.text() +
                         "\n" + sd::sha256_hex(canonical);
    const auto key = sigv4::derive_signing_key(secret_access_key, scope);
    const auto signature = sd::to_hex(sd::hmac_sha256(key, to_sign));
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(h.size() + 1);
    for (auto& [k, v] : h) {
        out.emplace_back(k == "host" ? "Host" : k, std::move(v));
    }
    out.emplace_back("Authorization", std::string(sigv4::k_algorithm) + " Credential=" +
                                          std::string(access_key_id) + "/" + scope.text() +
                                          ", SignedHeaders=" + names + ", Signature=" + signature);
    return out;
}

/// An answer's error: its `__type` and message, with the HTTP status.
[[nodiscard]] inline auto answer_error(const kinesis_http_answer& a) -> aws_json::error {
    aws_json::error e;
    e.status = a.status;
    e.type = "HTTP " + std::to_string(a.status);
    try {
        const auto v = boost::json::parse(a.body);
        if (const auto* o = v.if_object()) {
            if (const auto* t = o->if_contains("__type"); t != nullptr && t->is_string()) {
                e.type = std::string(t->get_string());
            }
            for (const auto* k : {"message", "Message"}) {
                if (const auto* m = o->if_contains(k); m != nullptr && m->is_string()) {
                    e.message = std::string(m->get_string());
                }
            }
        }
    } catch (const std::exception&) {
        // Not JSON: the status says enough.
    }
    return e;
}

}  // namespace global_tables_detail

/// A `replication_send` over HTTP(S) to one peer region; `transport`
/// replaces the socket in tests.
[[nodiscard]] inline auto http_replication_send(replication_peer_config cfg,
                                                kinesis_http_transport transport = {})
    -> replication_send {
    if (!transport) {
        kinesis_http_sink_config http;
        http.tls = cfg.tls;
        http.ca_path = cfg.ca_path;
        http.timeout = cfg.timeout;
        transport = [http](const std::string& host_port,
                           const std::vector<std::pair<std::string, std::string>>& headers,
                           const std::string& body) {
            return kinesis_destination_detail::http_post(
                http, host_port, headers, body, k_replication_path, "kythira-dynamodb-replication");
        };
    }
    return [cfg = std::move(cfg), transport = std::move(transport)](
               const boost::json::object& request) -> aws_json::result<boost::json::object> {
        const auto [akid, secret] = global_tables_detail::replication_keys(cfg, request);
        const auto body = boost::json::serialize(request);
        const auto answer = transport(
            cfg.endpoint, global_tables_detail::signed_replication_headers(cfg, akid, secret, body),
            body);
        if (!answer) {
            aws_json::error e;
            e.status = 0;
            e.type = "Unreachable";
            e.message = "No answer from " + cfg.endpoint;
            return e;
        }
        if (answer->status != 200) {
            return global_tables_detail::answer_error(*answer);
        }
        try {
            auto v = boost::json::parse(answer->body);
            if (auto* o = v.if_object()) {
                return std::move(*o);
            }
        } catch (const std::exception&) {
            // Falls through.
        }
        aws_json::error e;
        e.status = 0;
        e.type = "Unreadable";
        e.message = "An unreadable answer from " + cfg.endpoint;
        return e;
    };
}

/// Serves the peer API (`k_replication_path`) over a backend: the side of
/// a global table that other clusters' replicators call.
template<dynamo_backend Backend> class replication_receiver {
public:
    using result = aws_json::result<boost::json::object>;

    explicit replication_receiver(Backend& backend) : _backend(backend) {}

    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        fe.set_replication_handler(
            [this](call_context& c, boost::json::object& in) { return handle(c, in); });
    }

    auto handle(call_context& ctx, boost::json::object& in) -> result {
        const auto action = api_text(in, "Action");
        // With per-account tables (task 28) a table is found in the
        // replication principal's own account, so the sender signs with
        // the owner's key and names the owner; a mismatch is the sender's
        // configuration, said plainly rather than as a missing table.
        if (const auto owner = api_text(in, "Owner");
            ctx.namespaced && !owner.empty() && owner != ctx.owner()) {
            return modeled("AccessDeniedException",
                           "The replication principal belongs to account " + ctx.owner() +
                               ", but the table belongs to account " + owner +
                               ": give the replicator that account's key");
        }
        if (action == "CreateReplica") {
            return create_replica(ctx, in);
        }
        if (action == "DeleteReplica") {
            return delete_replica(ctx, in);
        }
        if (action == "SetReplicas") {
            return set_replicas(ctx, in);
        }
        if (action == "Apply") {
            return apply(ctx, in);
        }
        return validation_error("Unknown replication Action");
    }

private:
    [[nodiscard]] static auto api_text(const boost::json::object& in, std::string_view member)
        -> std::string {
        const auto* v = in.if_contains(member);
        return v != nullptr && v->is_string() ? std::string(v->get_string()) : std::string();
    }

    static auto modeled(std::string_view type, std::string message) -> aws_json::error {
        aws_json::error e;
        e.type = dynamodb_error_style().service_prefix + std::string(type);
        e.message = std::move(message);
        return e;
    }

    static auto not_found(const std::string& name) -> aws_json::error {
        return modeled(
            "ResourceNotFoundException",
            "Requested resource not found: Table: " + call_context::bare_name(name) + " not found");
    }

    static auto busy(std::string message) -> aws_json::error {
        return modeled("ResourceInUseException", std::move(message));
    }

    static auto from_catalog(const catalog_result& r) -> aws_json::error {
        aws_json::error e;
        e.type = r.error_type;
        e.message = r.error_message;
        return e;
    }

    /// Creates the table as a replica of the source's: this region's, with
    /// the source and its other replicas as its own, all ACTIVE.
    auto create_replica(call_context& ctx, const boost::json::object& in) -> result {
        const auto name = ctx.table_name(api_text(in, "TableName"));
        const auto source = api_text(in, "SourceRegion");
        const auto raw = aws_json::base64_decode(api_text(in, "Descriptor"));
        if (source.empty() || !raw) {
            return validation_error("CreateReplica needs SourceRegion and Descriptor");
        }
        auto d = decode_table_descriptor(
            std::string_view(reinterpret_cast<const char*>(raw->data()), raw->size()));
        // The source's catalog name may carry its account; the replica's
        // is this cluster's name for the table.
        if (!d || call_context::bare_name(d->name) != call_context::bare_name(name)) {
            return validation_error("CreateReplica's Descriptor is unreadable");
        }
        d->name = name;
        if (const auto have = _backend.describe(name)) {
            // A repeated call (the replicator tries again until the table
            // takes writes) finds the replica it made.
            if (have->global() && have->region == ctx.region && have->replica_in(source)) {
                return boost::json::object{
                    {"TableStatus", have->status == table_status::active ? "ACTIVE" : "CREATING"}};
            }
            // Not the replica, so not worth waiting for: a refusal.
            return validation_error("Table already exists and is not a replica: " +
                                    call_context::bare_name(name));
        }
        std::vector<table_replica> replicas;
        for (const auto& r : d->replicas) {
            if (r.region != ctx.region) {
                replicas.push_back(r);
            }
        }
        replicas.push_back(table_replica{.region = source});
        for (auto& r : replicas) {
            r.status = replica_status::active;
            r.backfilled = true;
            r.failure.clear();
            r.cursors.clear();
        }
        d->region = ctx.region;
        d->replicas = std::move(replicas);
        d->restore = {};
        d->policies.clear();
        d->deletion_protection = false;
        catalog_command c;
        c.op = catalog_command::kind::create_table;
        c.table = std::move(*d);
        c.now_ms = _backend.now_ms();
        const auto r = _backend.catalog(c);
        if (!r.ok()) {
            return from_catalog(r);
        }
        return boost::json::object{{"TableStatus", "CREATING"}};
    }

    auto delete_replica(const call_context& ctx, const boost::json::object& in) -> result {
        catalog_command c;
        c.op = catalog_command::kind::delete_table;
        c.table.name = ctx.table_name(api_text(in, "TableName"));
        c.now_ms = _backend.now_ms();
        const auto r = _backend.catalog(c);
        if (!r.ok() && !r.error_type.ends_with("ResourceNotFoundException")) {
            return from_catalog(r);
        }
        return boost::json::object{};
    }

    /// A peer's notice that a replica was added or removed elsewhere.
    auto set_replicas(call_context& ctx, const boost::json::object& in) -> result {
        const auto name = ctx.table_name(api_text(in, "TableName"));
        const auto source = api_text(in, "SourceRegion");
        const auto add = api_text(in, "Add");
        const auto remove = api_text(in, "Remove");
        if (add.empty() == remove.empty()) {
            return validation_error("SetReplicas needs one of Add and Remove");
        }
        const auto d = _backend.describe(name);
        if (!d) {
            return not_found(name);
        }
        catalog_command c;
        c.op = catalog_command::kind::set_replicas;
        c.table = *d;
        c.now_ms = _backend.now_ms();
        if (!add.empty()) {
            if (add == ctx.region || d->replica_in(add) != nullptr) {
                return boost::json::object{};
            }
            if (!d->global() || d->strong || d->replica_in(source) == nullptr) {
                return validation_error("Table " + call_context::bare_name(name) +
                                        " is not a replica of " + source);
            }
            // Start it where this cluster is with the source: what has not
            // reached the source yet goes to the new replica too.
            table_replica r{.region = add, .status = replica_status::active, .backfilled = true};
            r.cursors = d->replica_in(source)->cursors;
            c.table.replicas.push_back(std::move(r));
            const auto res = _backend.catalog(c);
            if (!res.ok()) {
                return from_catalog(res);
            }
            return boost::json::object{};
        }
        if (d->replica_in(remove) == nullptr) {
            return boost::json::object{};
        }
        std::erase_if(c.table.replicas, [&](const table_replica& r) { return r.region == remove; });
        // The first entry marks it DELETING, the second drops it: the
        // replica that asked deletes the peer's table, not this one.
        for (int i = 0; i < 2; ++i) {
            const auto res = _backend.catalog(c);
            if (!res.ok()) {
                return from_catalog(res);
            }
        }
        return boost::json::object{};
    }

    /// Writes from another replica, each applied if its stamp is newer.
    auto apply(call_context& ctx, const boost::json::object& in) -> result {
        const auto name = ctx.table_name(api_text(in, "TableName"));
        const auto source = api_text(in, "SourceRegion");
        const auto d = _backend.describe(name);
        if (!d) {
            return not_found(name);
        }
        if (d->status != table_status::active) {
            return busy("Table is not ACTIVE: " + call_context::bare_name(name));
        }
        if (!d->global() || d->strong || d->replica_in(source) == nullptr) {
            return validation_error("Table " + call_context::bare_name(name) +
                                    " is not a replica of " + source);
        }
        const auto* writes = in.if_contains("Writes");
        if (writes == nullptr || !writes->is_array()) {
            return validation_error("Apply needs Writes");
        }
        bool deletes = false;
        std::vector<item_command> commands;
        commands.reserve(writes->as_array().size());
        const auto gsis = maintained_indexes(d->gsis);
        for (const auto& v : writes->as_array()) {
            if (!v.is_object()) {
                return validation_error("A write must be an object");
            }
            const auto& w = v.as_object();
            const auto* del = w.if_contains("Delete");
            const bool deleted = del != nullptr && del->is_bool() && del->as_bool();
            const auto* body = w.if_contains(deleted ? "Key" : "Item");
            const auto* ms = w.if_contains("WrittenMs");
            const auto region = api_text(w, "Region");
            if (body == nullptr || !body->is_object() || ms == nullptr || !ms->is_number() ||
                region.empty()) {
                return validation_error("A write needs Item or Key, WrittenMs and Region");
            }
            auto it = item_from_json(body->as_object());
            if (!it) {
                return it.error();
            }
            std::uint64_t written = 0;
            try {
                written = ms->to_number<std::uint64_t>();
            } catch (const std::exception&) {
                return validation_error("WrittenMs must be a whole number");
            }
            if (written == 0) {
                return validation_error("WrittenMs must be positive");
            }
            deletes = deletes || deleted;
            item_command c;
            c.op = item_command::kind::replicate;
            c.table_id = d->table_id;
            c.schema = d->schema;
            c.payload = std::move(*it);
            c.replica_delete = deleted;
            c.lsis = d->lsis;
            c.gsis = gsis;
            c.log_changes = d->logs_changes();
            c.stream_ms = d->stream_label();
            c.region = region;
            c.written_ms = written;
            commands.push_back(std::move(c));
        }
        if (deletes && ctx.authorize) {
            aws_auth::authz_request r;
            r.action = "dynamodb:DeleteItem";
            r.resource = ctx.table_arn(name);
            if (auto denied = ctx.authorize(std::move(r))) {
                return *denied;
            }
        }
        std::size_t applied = 0;
        for (const auto& c : commands) {
            const auto r = _backend.write(c);
            if (r.outcome == item_result::status::error) {
                aws_json::error e;
                e.type = r.error_type;
                e.message = r.error_message;
                return e;
            }
            if (r.outcome != item_result::status::ok) {
                return busy("A replicated write was not applied; try again");
            }
            ++applied;
        }
        return boost::json::object{{"Applied", applied}};
    }

    Backend& _backend;
};

/// Keeps a cluster's global tables' replicas in step (see the file
/// comment); run on the node that leads the catalog.
template<dynamo_backend Backend>
requires change_feed_backend<Backend> && replica_backend<Backend>
class global_table_replicator {
public:
    struct options {
        /// Records read from a store stream at a time.
        std::size_t page_records = 500;
        /// The most bytes of writes one `Apply` carries (one write may
        /// exceed it).
        std::size_t batch_bytes = std::size_t{1} << 20U;
        std::function<void(std::string_view)> log;
    };

    global_table_replicator(Backend& backend, const peer_set* peers, options opts = {})
        : _backend(backend), _peers(peers), _options(std::move(opts)) {}

    void set_peers(const peer_set* peers) { _peers = peers; }

    void set_log(std::function<void(std::string_view)> log) { _options.log = std::move(log); }

    /// One pass over every global table; returns the replicas it moved on
    /// (a step taken, or writes sent).
    auto pass() -> std::size_t {
        std::size_t moved = 0;
        for (const auto& name : _backend.list_tables("", SIZE_MAX)) {
            const auto d = _backend.describe(name);
            // A strongly consistent table (task 27) replicates through the
            // strong host's Raft groups, not from here.
            if (!d || !d->global() || d->strong || d->status != table_status::active) {
                continue;
            }
            for (const auto& r : d->replicas) {
                try {
                    if (step(*d, r)) {
                        ++moved;
                    }
                } catch (const std::exception& e) {
                    say(name + " to " + r.region + ": " + e.what());
                }
            }
        }
        return moved;
    }

private:
    using cursor_map = std::map<std::string, std::uint64_t>;

    auto step(const table_descriptor& d, const table_replica& r) -> bool {
        const bool linked = _peers != nullptr && _peers->has(r.region);
        switch (r.status) {
            case replica_status::creating:
                if (!linked) {
                    return finish(d, r.region, replica_status::creation_failed,
                                  "No peer link for region " + r.region);
                }
                return r.backfilled ? join(d, r) : backfill(d, r);
            case replica_status::active:
                if (!linked) {
                    return false;
                }
                return tail(d, r, /*any_origin=*/false).sent > 0;
            case replica_status::deleting:
                return remove(d, r, linked);
            case replica_status::creation_failed:
                return false;
        }
        return false;
    }

    void say(const std::string& line) const {
        if (_options.log) {
            _options.log("global tables: " + line);
        }
    }

    /// Sends `request` about table `d` to `region`. With per-account
    /// tables the request names the table's owner as `Owner`, which picks
    /// the key it is signed with (`replication_peer_config::account_keys`)
    /// and lets the peer say so when that key is another account's.
    auto call(const table_descriptor& d, const std::string& region, boost::json::object request)
        -> aws_json::result<boost::json::object> {
        if (_peers == nullptr) {
            aws_json::error e;
            e.status = 0;
            e.message = "no peers";
            return e;
        }
        if (const auto slash = d.name.find('/'); slash != std::string::npos) {
            request["Owner"] = d.name.substr(0, slash);
        }
        return _peers->send(region, request);
    }

    /// The table's change log streams, parents (older epochs) first.
    auto shards_of(const table_descriptor& d) -> std::vector<change_shard> {
        auto shards = _backend.change_shards(table_range(d.table_id));
        std::ranges::sort(shards, [](const change_shard& a, const change_shard& b) {
            const auto ea = streams_detail::epoch_of(a.stream.key);
            const auto eb = streams_detail::epoch_of(b.stream.key);
            return ea != eb ? ea < eb : a.stream.key < b.stream.key;
        });
        return shards;
    }

    /// Sends `writes` as one `Apply`.
    auto send(const table_descriptor& d, const std::string& region, boost::json::array writes)
        -> aws_json::result<boost::json::object> {
        return call(d, region,
                    boost::json::object{{"Action", "Apply"},
                                        {"TableName", call_context::bare_name(d.name)},
                                        {"SourceRegion", d.region},
                                        {"Writes", std::move(writes)}});
    }

    /// The replica's entry as the catalog should have it.
    auto set_replica(const table_descriptor& d, const std::string& region,
                     const std::function<void(table_replica&)>& change) -> bool {
        catalog_command c;
        c.op = catalog_command::kind::set_replicas;
        c.table = d;
        auto* r = global_tables_detail::replica_named(c.table.replicas, region);
        if (r == nullptr) {
            return false;
        }
        change(*r);
        c.now_ms = _backend.now_ms();
        const auto res = _backend.catalog(c);
        if (!res.ok()) {
            say(d.name + ": " + res.error_message);
            return false;
        }
        return true;
    }

    auto finish(const table_descriptor& d, const std::string& region, replica_status status,
                std::string failure = {}) -> bool {
        if (!failure.empty()) {
            say(d.name + " to " + region + ": " + failure);
        }
        return set_replica(d, region, [&](table_replica& r) {
            r.status = status;
            r.failure = std::move(failure);
        });
    }

    auto save_cursors(const table_descriptor& d, const std::string& region,
                      const cursor_map& cursors) -> void {
        catalog_command c;
        c.op = catalog_command::kind::replica_progress;
        c.table.name = d.name;
        table_replica r{.region = region};
        r.cursors.assign(cursors.begin(), cursors.end());
        c.table.replicas.push_back(std::move(r));
        c.now_ms = _backend.now_ms();
        const auto res = _backend.catalog(c);
        if (!res.ok()) {
            say(d.name + ": " + res.error_message);
        }
    }

    /// CREATING, first part: create the peer's table and copy the items.
    auto backfill(const table_descriptor& d, const table_replica& r) -> bool {
        const auto created = call(
            d, r.region,
            boost::json::object{{"Action", "CreateReplica"},
                                {"TableName", call_context::bare_name(d.name)},
                                {"SourceRegion", d.region},
                                {"Descriptor", detail::base64_of(encode_table_descriptor(d))}});
        if (!created) {
            if (global_tables_detail::retryable(created.error())) {
                return false;
            }
            return finish(d, r.region, replica_status::creation_failed,
                          created.error().message.value_or(created.error().type));
        }
        // Positions first: a write after them is in the log and is sent
        // when the replica joins, over the copy made below.
        cursor_map positions;
        for (const auto& shard : shards_of(d)) {
            const auto page = _backend.read_changes(shard.stream.key, 0, 0);
            positions[shard.stream.key] = page.known ? page.last_seq + 1 : shard.stream.first_seq;
        }
        boost::json::array batch;
        std::size_t bytes = 0;
        std::optional<aws_json::error> failed;
        const auto flush = [&] {
            if (batch.empty() || failed) {
                return;
            }
            auto res = send(d, r.region, std::move(batch));
            batch = {};
            bytes = 0;
            if (!res) {
                failed = res.error();
            }
        };
        _backend.read_stamped_range(
            table_range(d.table_id),
            [&](std::string_view, const item& it, const std::optional<replica_stamp>& stamp) {
                // An item no global write has touched is older than any
                // that has.
                auto w = global_tables_detail::write_json(
                    it, false, stamp.value_or(replica_stamp{1, d.region}));
                bytes += global_tables_detail::json_size(w);
                batch.push_back(std::move(w));
                if (bytes >= _options.batch_bytes) {
                    flush();
                }
                return !failed;
            });
        flush();
        if (failed) {
            // The table may still be being created there; the copy starts
            // over on the next pass.
            if (!global_tables_detail::retryable(*failed)) {
                say(d.name + " to " + r.region + ": " + failed->message.value_or(failed->type));
            }
            return false;
        }
        save_cursors(d, r.region, positions);
        return set_replica(d, r.region, [](table_replica& x) { x.backfilled = true; });
    }

    /// CREATING, second part: tell the other replicas, send what came
    /// since the copy, and go ACTIVE.
    auto join(const table_descriptor& d, const table_replica& r) -> bool {
        for (const auto& other : d.replicas) {
            if (other.region == r.region || other.status != replica_status::active) {
                continue;
            }
            const auto told =
                call(d, other.region,
                     boost::json::object{{"Action", "SetReplicas"},
                                         {"TableName", call_context::bare_name(d.name)},
                                         {"SourceRegion", d.region},
                                         {"Add", r.region}});
            if (!told && !global_tables_detail::not_found(told.error())) {
                if (!global_tables_detail::retryable(told.error())) {
                    say(d.name + ": telling " + other.region + " of " + r.region + ": " +
                        told.error().message.value_or(told.error().type));
                }
                return false;
            }
        }
        // Everything this cluster holds, from any region: a write another
        // replica sent here before it heard of the new one reaches it so.
        const auto t = tail(d, r, /*any_origin=*/true);
        if (!t.complete) {
            return t.sent > 0;
        }
        return finish(d, r.region, replica_status::active);
    }

    /// DELETING: delete the peer's table, tell the others, drop it.
    auto remove(const table_descriptor& d, const table_replica& r, bool linked) -> bool {
        if (linked) {
            const auto gone =
                call(d, r.region,
                     boost::json::object{{"Action", "DeleteReplica"},
                                         {"TableName", call_context::bare_name(d.name)}});
            if (!gone && !global_tables_detail::not_found(gone.error())) {
                say(d.name + ": deleting the replica in " + r.region + ": " +
                    gone.error().message.value_or(gone.error().type));
                return false;
            }
        }
        for (const auto& other : d.replicas) {
            if (other.region == r.region || other.status == replica_status::deleting ||
                _peers == nullptr || !_peers->has(other.region)) {
                continue;
            }
            const auto told =
                call(d, other.region,
                     boost::json::object{{"Action", "SetReplicas"},
                                         {"TableName", call_context::bare_name(d.name)},
                                         {"SourceRegion", d.region},
                                         {"Remove", r.region}});
            if (!told && !global_tables_detail::not_found(told.error())) {
                return false;
            }
        }
        catalog_command c;
        c.op = catalog_command::kind::set_replicas;
        c.table = d;
        std::erase_if(c.table.replicas,
                      [&](const table_replica& x) { return x.region == r.region; });
        c.now_ms = _backend.now_ms();
        const auto res = _backend.catalog(c);
        if (!res.ok()) {
            say(d.name + ": " + res.error_message);
            return false;
        }
        return true;
    }

    struct tail_result {
        std::size_t sent = 0;
        /// Every stream was read to its end and every write sent.
        bool complete = false;
    };

    /// Sends the log's records since the replica's cursors: this region's
    /// writes, or (`any_origin`) every write but the replica's own.
    auto tail(const table_descriptor& d, const table_replica& r, bool any_origin) -> tail_result {
        tail_result out;
        cursor_map cursors(r.cursors.begin(), r.cursors.end());
        const auto before = cursors;
        const auto shards = shards_of(d);
        std::set<std::string> listed;
        for (const auto& shard : shards) {
            listed.insert(shard.stream.key);
        }
        std::set<std::string> unfinished;
        bool stopped = false;
        for (const auto& shard : shards) {
            const auto& key = shard.stream.key;
            if (stopped || std::ranges::any_of(shard.stream.parents, [&](const auto& p) {
                    return listed.contains(p) && unfinished.contains(p);
                })) {
                unfinished.insert(key);
                continue;
            }
            const bool tracked = cursors.contains(key);
            auto at = tracked ? cursors[key] : shard.stream.first_seq;
            boost::json::array batch;
            std::size_t bytes = 0;
            std::uint64_t batch_end = at;
            const auto flush = [&]() -> bool {
                if (batch.empty()) {
                    cursors[key] = batch_end;
                    return true;
                }
                const auto n = batch.size();
                auto res = send(d, r.region, std::move(batch));
                batch = {};
                bytes = 0;
                if (!res) {
                    if (!global_tables_detail::retryable(res.error())) {
                        say(d.name + " to " + r.region + ": " +
                            res.error().message.value_or(res.error().type));
                    }
                    return false;
                }
                out.sent += n;
                cursors[key] = batch_end;
                return true;
            };
            bool open = true;
            for (;;) {
                const auto page = _backend.read_changes(key, at, _options.page_records);
                if (!page.known) {
                    open = false;
                    break;
                }
                open = page.open;
                if (at < page.first_seq) {
                    if (tracked || page.first_seq > shard.stream.first_seq) {
                        say(d.name + " to " + r.region + ": records " + std::to_string(at) +
                            " to " + std::to_string(page.first_seq - 1) + " of " + key +
                            " were trimmed before they were sent");
                    }
                    at = page.first_seq;
                    batch_end = at;
                }
                if (page.records.empty()) {
                    break;
                }
                for (const auto& [seq, rec] : page.records) {
                    at = seq + 1;
                    const bool mine = rec.origin.empty() || rec.origin == d.region;
                    if (rec.table_id == d.table_id &&
                        (any_origin ? rec.origin != r.region : mine)) {
                        const replica_stamp stamp{rec.written_ms != 0 ? rec.written_ms : 1,
                                                  rec.origin.empty() ? d.region : rec.origin};
                        auto w =
                            rec.new_image
                                ? global_tables_detail::write_json(*rec.new_image, false, stamp)
                                : global_tables_detail::write_json(rec.keys, true, stamp);
                        bytes += global_tables_detail::json_size(w);
                        batch.push_back(std::move(w));
                    }
                    batch_end = at;
                    if (bytes >= _options.batch_bytes && !flush()) {
                        stopped = true;
                        break;
                    }
                }
                if (stopped) {
                    break;
                }
            }
            if (!stopped && !flush()) {
                stopped = true;
            }
            if (stopped || open) {
                unfinished.insert(key);
            }
        }
        // Streams no longer listed were trimmed after being read.
        std::erase_if(cursors, [&](const auto& kv) { return !listed.contains(kv.first); });
        if (cursors != before) {
            save_cursors(d, r.region, cursors);
        }
        out.complete = !stopped;
        return out;
    }

    Backend& _backend;
    const peer_set* _peers;
    options _options;
};

}  // namespace kythira::dynamodb
