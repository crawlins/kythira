// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// Point-in-time recovery (`.kiro/specs/dynamodb-compatible-api/` task 19,
/// Requirement 17.2): `UpdateContinuousBackups`,
/// `DescribeContinuousBackups` and `RestoreTableToPointInTime`, over the
/// backup store of task 18 and the change log of task 15.
///
/// While recovery is enabled the table logs its changes, and the archiver
/// (`pitr_worker`, on the catalog leader beside the backup worker) keeps,
/// under `pitr/<table id>/<enable time>/` in the backup store:
///
/// - `log/<n>.json`: the table's change records as DynamoDB JSON lines,
///   one object per pass that found any, numbered in the order written. A
///   store stream continued by a split is archived only once its parent is
///   drained, so the archive holds every item's records in order.
/// - `base/<start>/`: a base, every item read consistently (the backup
///   worker's layout), with `base.json` naming its start and end times and
///   the first log object written after it started. One is taken when
///   recovery is enabled and another each `base_interval`.
///
/// A restore to time T loads the newest base that ended by T, then replays
/// every archived record from that base's first log object on whose write
/// time is at most T: a put of its new image, or a delete of its key. A
/// record already in the base is replayed harmlessly, since replay keeps
/// each item's order and ends at its last write by T. The archive reaches
/// `LatestRestorableDateTime` once a pass has caught up with every store
/// stream; bases and log objects older than the recovery period (bar the
/// newest base before it, which restores to its start) are removed.
///
/// Times are the clocks of the nodes that made the writes (each record's
/// write time) and of the catalog leader (bases, the latest restorable
/// time), so restores assume those clocks agree to well under a second.

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/backup.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/change_log.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/key_layout.hpp>
#include <kythira/dynamodb/streams.hpp>
#include <kythira/dynamodb/table_api.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

/// A base of a point-in-time recovery archive (`base.json`).
struct pitr_base {
    /// When the read began and ended (ms since the epoch, catalog leader).
    std::uint64_t start_ms = 0;
    std::uint64_t end_ms = 0;
    /// The first log object written after the read began.
    std::uint64_t log_from = 0;
    std::uint64_t items = 0;
    std::vector<backup_object> objects;

    friend auto operator==(const pitr_base&, const pitr_base&) -> bool = default;
};

namespace pitr_detail {

inline constexpr std::string_view k_prefix = "pitr/";
inline constexpr std::uint64_t k_day_ms = 86'400'000;

[[nodiscard]] inline auto digits(std::uint64_t v, int width) -> std::string {
    std::array<char, 32> buf{};
    const auto n =
        std::snprintf(buf.data(), buf.size(), "%0*llu", width, static_cast<unsigned long long>(v));
    return {buf.data(), static_cast<std::size_t>(n)};
}

/// Every archive of `table_id`.
[[nodiscard]] inline auto table_prefix(std::uint64_t table_id) -> std::string {
    return std::string(k_prefix) + digits(table_id, 20) + "/";
}
/// The archive that the enable at `enabled_ms` started.
[[nodiscard]] inline auto archive_prefix(std::uint64_t table_id, std::uint64_t enabled_ms)
    -> std::string {
    return table_prefix(table_id) + digits(enabled_ms, 14) + "/";
}
[[nodiscard]] inline auto log_key(const std::string& archive, std::uint64_t n) -> std::string {
    return archive + "log/" + digits(n, 16) + ".json";
}
[[nodiscard]] inline auto base_prefix(const std::string& archive, std::uint64_t start_ms)
    -> std::string {
    return archive + "base/" + digits(start_ms, 14) + "/";
}

/// The number of a log object's key, or nothing.
[[nodiscard]] inline auto log_number(std::string_view key) -> std::optional<std::uint64_t> {
    const auto at = key.rfind("/log/");
    if (at == std::string_view::npos || !key.ends_with(".json")) {
        return std::nullopt;
    }
    const auto n = key.substr(at + 5, key.size() - at - 5 - 5);
    if (n.size() != 16 || !std::ranges::all_of(n, [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    return std::stoull(std::string(n));
}

/// The table id and enable time an archive key names, or nothing.
[[nodiscard]] inline auto archive_of(std::string_view key)
    -> std::optional<std::pair<std::uint64_t, std::uint64_t>> {
    if (!key.starts_with(k_prefix)) {
        return std::nullopt;
    }
    const auto rest = key.substr(k_prefix.size());
    if (rest.size() < 36 || rest[20] != '/' || rest[35] != '/') {
        return std::nullopt;
    }
    const auto id = rest.substr(0, 20);
    const auto at = rest.substr(21, 14);
    const auto numeric = [](std::string_view s) {
        return std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
    };
    if (!numeric(id) || !numeric(at)) {
        return std::nullopt;
    }
    return std::pair{std::stoull(std::string(id)), std::stoull(std::string(at))};
}

[[nodiscard]] inline auto encode_base(const pitr_base& b) -> std::string {
    boost::json::array objects;
    for (const auto& o : b.objects) {
        objects.push_back(boost::json::object{
            {"key", o.key}, {"items", o.items}, {"bytes", o.bytes}, {"sha256", o.sha256}});
    }
    return boost::json::serialize(boost::json::object{
        {"formatVersion", 1},
        {"startMs", b.start_ms},
        {"endMs", b.end_ms},
        {"logFrom", b.log_from},
        {"itemCount", b.items},
        {"objects", std::move(objects)},
    });
}

[[nodiscard]] inline auto decode_base(std::string_view text) -> std::optional<pitr_base> {
    try {
        const auto v = boost::json::parse(text);
        const auto& o = v.as_object();
        if (o.at("formatVersion").to_number<int>() != 1) {
            return std::nullopt;
        }
        pitr_base b;
        b.start_ms = o.at("startMs").to_number<std::uint64_t>();
        b.end_ms = o.at("endMs").to_number<std::uint64_t>();
        b.log_from = o.at("logFrom").to_number<std::uint64_t>();
        b.items = o.at("itemCount").to_number<std::uint64_t>();
        for (const auto& e : o.at("objects").as_array()) {
            const auto& obj = e.as_object();
            b.objects.push_back(backup_object{std::string(obj.at("key").as_string()),
                                              obj.at("items").to_number<std::uint64_t>(),
                                              obj.at("bytes").to_number<std::uint64_t>(),
                                              std::string(obj.at("sha256").as_string())});
        }
        return b;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// The finished bases of an archive, oldest first.
[[nodiscard]] inline auto bases(backup_store& store, const std::string& archive)
    -> std::vector<pitr_base> {
    std::vector<pitr_base> out;
    for (const auto& key : store.list(archive + "base/")) {
        if (!key.ends_with("/base.json")) {
            continue;
        }
        if (const auto text = store.get(key)) {
            if (auto b = decode_base(*text)) {
                out.push_back(std::move(*b));
            }
        }
    }
    std::ranges::sort(out, {}, &pitr_base::start_ms);
    return out;
}

/// An archive's log objects, by number.
[[nodiscard]] inline auto log_objects(backup_store& store, const std::string& archive)
    -> std::map<std::uint64_t, std::string> {
    std::map<std::uint64_t, std::string> out;
    for (auto& key : store.list(archive + "log/")) {
        if (const auto n = log_number(key)) {
            out.emplace(*n, std::move(key));
        }
    }
    return out;
}

/// One archived change record: its write time, the item's key, and its new
/// image (none for a delete).
[[nodiscard]] inline auto record_line(const change_record& rec) -> std::string {
    boost::json::object o{
        {"WrittenMs", rec.written_ms},
        {"Keys", item_to_json(rec.keys)},
    };
    if (rec.new_image) {
        o["NewImage"] = item_to_json(*rec.new_image);
    }
    auto out = boost::json::serialize(o);
    out += '\n';
    return out;
}

/// Replays the archives of `table_id`: the newest base that ended by
/// `base_by_ms` (`on_item` with each of its items, as a DynamoDB JSON
/// map), then every archived record after it whose write time is at most
/// `upto_ms` (`on_record` with each, as `record_line` wrote it), in the
/// order written. Throws when no base ended by `base_by_ms`, or when an
/// object the base or the log names is missing or damaged.
template<typename OnItem, typename OnRecord>
void replay(backup_store& store, std::uint64_t table_id, std::uint64_t base_by_ms,
            std::uint64_t upto_ms, OnItem on_item, OnRecord on_record) {
    std::set<std::uint64_t> archives;
    for (const auto& key : store.list(table_prefix(table_id))) {
        if (const auto owner = archive_of(key)) {
            archives.insert(owner->second);
        }
    }
    std::optional<pitr_base> base;
    std::string archive;
    for (auto it = archives.rbegin(); it != archives.rend() && !base; ++it) {
        archive = archive_prefix(table_id, *it);
        for (auto& b : bases(store, archive)) {
            if (b.end_ms <= base_by_ms) {
                base = std::move(b);
            }
        }
    }
    if (!base) {
        throw std::runtime_error("no point-in-time recovery base of table " +
                                 std::to_string(table_id) + " ended by " +
                                 std::to_string(base_by_ms));
    }
    for (const auto& o : base->objects) {
        backup_detail::read_object(store, o, [&](std::string_view line) {
            on_item(boost::json::parse(line).as_object().at("Item").as_object());
        });
    }
    for (const auto& [n, key] : log_objects(store, archive)) {
        if (n < base->log_from) {
            continue;
        }
        const auto body = store.get(key);
        if (!body) {
            throw std::runtime_error("archive object " + key + " is missing");
        }
        std::string_view rest(*body);
        while (!rest.empty()) {
            const auto nl = rest.find('\n');
            const auto line = rest.substr(0, nl);
            rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
            if (line.empty()) {
                continue;
            }
            const auto v = boost::json::parse(line);
            const auto& rec = v.as_object();
            if (rec.at("WrittenMs").to_number<std::uint64_t>() <= upto_ms) {
                on_record(rec);
            }
        }
    }
}

[[nodiscard]] inline auto ms_of_seconds(double seconds) -> std::uint64_t {
    return seconds <= 0 ? 0 : static_cast<std::uint64_t>(std::llround(seconds * 1000.0));
}

[[nodiscard]] inline auto modeled(std::string_view type, std::string message) -> aws_json::error {
    return table_api_detail::modeled(type, std::move(message));
}

[[nodiscard]] inline auto not_configured() -> aws_json::error {
    return modeled("ContinuousBackupsUnavailableException",
                   "Backups are not configured on this cluster: set KYTHIRA_DYNAMODB_BACKUP_DIR "
                   "or KYTHIRA_DYNAMODB_BACKUP_S3_BUCKET");
}

/// The earliest time a restore may name, given the window: 0 when there
/// is none yet.
[[nodiscard]] inline auto earliest(const continuous_backups& p, std::uint64_t now_ms)
    -> std::uint64_t {
    if (!p.enabled || p.earliest_ms == 0) {
        return 0;
    }
    const auto window = static_cast<std::uint64_t>(p.recovery_days) * k_day_ms;
    return std::max(p.earliest_ms, now_ms > window ? now_ms - window : 0);
}

}  // namespace pitr_detail

// -- The API ------------------------------------------------------------------

/// The three point-in-time recovery actions.
template<dynamo_backend Backend> class pitr_api {
public:
    using result = aws_json::result<boost::json::object>;

    pitr_api(Backend& backend, std::shared_ptr<backup_store> store)
        : _backend(backend), _store(std::move(store)) {}

    void set_store(std::shared_ptr<backup_store> store) { _store = std::move(store); }
    /// Called after a change the archiver should see at once.
    void on_changed(std::function<void()> fn) { _changed = std::move(fn); }

    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        fe.on("UpdateContinuousBackups",
              [this](call_context& c, boost::json::object& in) { return update(c, in); });
        fe.on("DescribeContinuousBackups",
              [this](call_context& c, boost::json::object& in) { return describe(c, in); });
        fe.on("RestoreTableToPointInTime",
              [this](call_context& c, boost::json::object& in) { return restore(c, in); });
    }

    auto update(call_context& /*ctx*/, boost::json::object& in) -> result {
        namespace pd = pitr_detail;
        const auto name = table_api_detail::table_name_of(in.at("TableName").as_string());
        const auto& spec = in.at("PointInTimeRecoverySpecification").as_object();
        const bool enable = spec.at("PointInTimeRecoveryEnabled").as_bool();
        std::uint32_t days = 35;
        if (const auto* v = spec.if_contains("RecoveryPeriodInDays");
            v != nullptr && v->is_number()) {
            if (!enable) {
                return validation_error(
                    "RecoveryPeriodInDays can only be specified when PointInTimeRecoveryEnabled "
                    "is true");
            }
            days = v->to_number<std::uint32_t>();
        }
        const auto d = _backend.describe(name);
        if (!d) {
            return pd::modeled("TableNotFoundException", "Table not found: " + name);
        }
        if (!_store) {
            return pd::not_configured();
        }
        catalog_command c;
        c.op = catalog_command::kind::set_pitr;
        c.table.name = d->name;
        c.table.pitr.enabled = enable;
        c.table.pitr.recovery_days = days;
        c.now_ms = _backend.now_ms();
        const auto res = _backend.catalog(c);
        if (!res.ok()) {
            return table_api_detail::error_of(res);
        }
        if (_changed) {
            _changed();
        }
        return boost::json::object{{"ContinuousBackupsDescription", description(*res.table)}};
    }

    auto describe(call_context& /*ctx*/, boost::json::object& in) -> result {
        const auto name = table_api_detail::table_name_of(in.at("TableName").as_string());
        const auto d = _backend.describe(name);
        if (!d) {
            return pitr_detail::modeled("TableNotFoundException", "Table not found: " + name);
        }
        return boost::json::object{{"ContinuousBackupsDescription", description(*d)}};
    }

    auto restore(call_context& ctx, boost::json::object& in) -> result {
        namespace pd = pitr_detail;
        namespace bd = backup_detail;
        const std::string target(in.at("TargetTableName").as_string());
        if (!table_api_detail::valid_table_name(target)) {
            return table_api_detail::bad_table_name();
        }
        std::string source = bd::text(in, "SourceTableName");
        if (source.empty()) {
            source = table_api_detail::table_name_of(bd::text(in, "SourceTableArn"));
        }
        if (source.empty()) {
            return validation_error("Either SourceTableArn or SourceTableName must be specified");
        }
        const auto* latest_flag = in.if_contains("UseLatestRestorableTime");
        const bool use_latest =
            latest_flag != nullptr && latest_flag->is_bool() && latest_flag->as_bool();
        const auto* at = in.if_contains("RestoreDateTime");
        const bool has_at = at != nullptr && at->is_number();
        if (use_latest && has_at) {
            return validation_error(
                "Only one of RestoreDateTime or UseLatestRestorableTime can be specified");
        }
        if (!use_latest && !has_at) {
            return validation_error(
                "Either RestoreDateTime or UseLatestRestorableTime must be specified");
        }
        const auto d = _backend.describe(source);
        if (!d) {
            return pd::modeled("TableNotFoundException", "Table not found: " + source);
        }
        if (!d->pitr.enabled) {
            return pd::modeled("PointInTimeRecoveryUnavailableException",
                               "Point in time recovery is not enabled for table '" + source + "'");
        }
        const auto earliest = pd::earliest(d->pitr, _backend.now_ms());
        const auto latest = d->pitr.latest_ms;
        if (earliest == 0 || latest < earliest) {
            return pd::modeled("InvalidRestoreTimeException",
                               "No restorable time is available yet for table '" + source + "'");
        }
        const auto point = use_latest ? latest : pd::ms_of_seconds(at->to_number<double>());
        if (point < earliest || point > latest) {
            return pd::modeled("InvalidRestoreTimeException",
                               "RestoreDateTime must be between " +
                                   std::to_string(table_api_detail::millis_to_seconds(earliest)) +
                                   " and " +
                                   std::to_string(table_api_detail::millis_to_seconds(latest)));
        }
        if (_backend.describe(target)) {
            return pd::modeled("TableAlreadyExistsException", "Table already exists: " + target);
        }
        auto t = bd::restored_descriptor(*d, target);
        if (auto e = bd::apply_overrides(in, t)) {
            return *e;
        }
        t.restore.source_table_arn = ctx.table_arn(d->name);
        t.restore.source_table_id = d->table_id;
        t.restore.point_ms = point;
        catalog_command c;
        c.op = catalog_command::kind::create_table;
        c.table = std::move(t);
        c.now_ms = _backend.now_ms();
        const auto res = _backend.catalog(c);
        if (!res.ok()) {
            if (res.error_type.ends_with("ResourceInUseException")) {
                return pd::modeled("TableAlreadyExistsException",
                                   "Table already exists: " + target);
            }
            return table_api_detail::error_of(res);
        }
        if (_changed) {
            _changed();
        }
        return boost::json::object{{"TableDescription", table_description(*res.table, ctx)}};
    }

private:
    [[nodiscard]] auto description(const table_descriptor& d) const -> boost::json::object {
        boost::json::object pitr{
            {"PointInTimeRecoveryStatus", d.pitr.enabled ? "ENABLED" : "DISABLED"}};
        if (d.pitr.enabled) {
            pitr["RecoveryPeriodInDays"] = d.pitr.recovery_days;
            const auto earliest = pitr_detail::earliest(d.pitr, _backend.now_ms());
            if (earliest != 0 && d.pitr.latest_ms >= earliest) {
                pitr["EarliestRestorableDateTime"] = table_api_detail::millis_to_seconds(earliest);
                pitr["LatestRestorableDateTime"] =
                    table_api_detail::millis_to_seconds(d.pitr.latest_ms);
            }
        }
        return boost::json::object{
            {"ContinuousBackupsStatus", _store ? "ENABLED" : "DISABLED"},
            {"PointInTimeRecoveryDescription", std::move(pitr)},
        };
    }

    Backend& _backend;
    std::shared_ptr<backup_store> _store;
    std::function<void()> _changed;
};

// -- The archiver -------------------------------------------------------------

struct pitr_worker_options {
    /// How far behind the start of a pass that caught up the latest
    /// restorable time is put, for writes stamped before it that commit
    /// after the pass read their stream.
    std::uint64_t latest_lag_ms = 1000;
    /// How often a new base is taken.
    std::uint64_t base_interval_ms = pitr_detail::k_day_ms;
    /// Change records read per page.
    std::size_t page_records = 1000;
    /// Change records looked at per table per pass.
    std::size_t max_records_per_pass = 20000;
    /// How often the store is swept for archives no table owns.
    std::chrono::milliseconds sweep_interval{60000};
};

/// Archives change records and takes bases for tables with point-in-time
/// recovery, prunes what has left their windows, and loads point-in-time
/// restores. Runs on the catalog leader.
template<typename Backend>
requires dynamo_backend<Backend> && change_feed_backend<Backend>
class pitr_worker {
public:
    pitr_worker(Backend& backend, std::shared_ptr<backup_store> store,
                pitr_worker_options options = {})
        : _backend(backend), _store(std::move(store)), _options(options) {}

    void set_store(std::shared_ptr<backup_store> store) { _store = std::move(store); }
    /// Sweep for orphaned archives on the next pass.
    void poke() { _poked.store(true); }

    /// One pass. Returns the change records archived and restores loaded.
    auto pass() -> std::size_t {
        if (!_store) {
            return 0;
        }
        std::size_t done = 0;
        std::vector<table_descriptor> tables;
        for (const auto& name : _backend.list_tables("", SIZE_MAX)) {
            if (auto d = _backend.describe(name)) {
                tables.push_back(std::move(*d));
            }
        }
        for (const auto& d : tables) {
            if (d.pitr.enabled && d.status != table_status::deleting) {
                done += serve(d);
            }
        }
        for (const auto& d : tables) {
            if (d.status == table_status::creating && d.restore.in_progress &&
                d.restore.source_backup_arn.empty() && d.restore.point_ms != 0) {
                load(d);
                ++done;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (_poked.exchange(false) || now >= _next_sweep) {
            _next_sweep = now + _options.sweep_interval;
            sweep(tables);
        }
        return done;
    }

private:
    using cursor_list = std::vector<std::pair<std::string, std::uint64_t>>;

    /// Archive, base and prune for one table.
    auto serve(const table_descriptor& d) -> std::size_t {
        namespace pd = pitr_detail;
        const auto archive = pd::archive_prefix(d.table_id, d.pitr.enabled_ms);
        auto next = d.pitr;
        const auto archived = archive_pass(d, archive, next);
        const auto now = _backend.now_ms();
        auto bases = pd::bases(*_store, archive);
        if (bases.empty() || now - bases.back().start_ms >= _options.base_interval_ms) {
            bases.push_back(take_base(d, archive, next.next_object));
        }
        prune(d, archive, bases, now);
        next.earliest_ms = std::max(next.earliest_ms, bases.front().end_ms);
        if (next != d.pitr) {
            catalog_command c;
            c.op = catalog_command::kind::pitr_progress;
            c.table.name = d.name;
            c.table.pitr = std::move(next);
            c.now_ms = now;
            (void)_backend.catalog(c);
        }
        return archived;
    }

    /// Copies the table's change records not yet archived into one log
    /// object, moving `next`'s cursors, object number and latest time.
    auto archive_pass(const table_descriptor& d, const std::string& archive,
                      continuous_backups& next) -> std::size_t {
        const auto started = _backend.now_ms();
        auto shards = _backend.change_shards(table_range(d.table_id));
        // Parents (older epochs) first.
        std::ranges::sort(shards, [](const change_shard& a, const change_shard& b) {
            const auto ea = streams_detail::epoch_of(a.stream.key);
            const auto eb = streams_detail::epoch_of(b.stream.key);
            return ea != eb ? ea < eb : a.stream.key < b.stream.key;
        });
        std::map<std::string, std::uint64_t> stored(d.pitr.cursors.begin(), d.pitr.cursors.end());
        std::map<std::string, std::uint64_t> cursors;
        std::set<std::string> listed;
        std::set<std::string> undrained;
        bool behind = false;
        for (const auto& s : shards) {
            listed.insert(s.stream.key);
        }
        std::size_t budget = _options.max_records_per_pass;
        std::string lines;
        std::size_t archived = 0;
        for (const auto& s : shards) {
            const auto& key = s.stream.key;
            auto at = std::max(stored.contains(key) ? stored[key] : s.stream.first_seq,
                               s.stream.first_seq);
            cursors[key] = at;
            const bool parent_pending = std::ranges::any_of(s.stream.parents, [&](const auto& p) {
                return listed.contains(p) && undrained.contains(p);
            });
            if (parent_pending || budget == 0) {
                undrained.insert(key);
                behind = true;
                continue;
            }
            bool caught_up = false;
            bool open = true;
            while (budget > 0) {
                const auto page =
                    _backend.read_changes(key, at, std::min(_options.page_records, budget));
                open = page.open;
                if (!page.known) {
                    caught_up = true;
                    open = false;
                    break;
                }
                if (page.records.empty()) {
                    caught_up = at > page.last_seq;
                    break;
                }
                for (const auto& [seq, rec] : page.records) {
                    --budget;
                    at = seq + 1;
                    if (rec.table_id == d.table_id && rec.written_ms >= d.pitr.enabled_ms) {
                        lines += pitr_detail::record_line(rec);
                        ++archived;
                    }
                    if (budget == 0) {
                        break;
                    }
                }
            }
            cursors[key] = at;
            if (!caught_up) {
                behind = true;
            }
            if (open || !caught_up) {
                undrained.insert(key);
            }
        }
        if (!lines.empty()) {
            // A pass whose progress never reached the catalog is redone
            // under the same number, from the same cursors.
            _store->put(pitr_detail::log_key(archive, next.next_object), lines);
            ++next.next_object;
        }
        next.cursors = cursor_list(cursors.begin(), cursors.end());
        if (!behind && started > _options.latest_lag_ms) {
            next.latest_ms = std::max(next.latest_ms, started - _options.latest_lag_ms);
        }
        return archived;
    }

    auto take_base(const table_descriptor& d, const std::string& archive, std::uint64_t log_from)
        -> pitr_base {
        namespace pd = pitr_detail;
        pitr_base b;
        b.start_ms = _backend.now_ms();
        b.log_from = log_from;
        const auto prefix = pd::base_prefix(archive, b.start_ms);
        auto got = backup_detail::write_items(
            _backend, *_store, d.table_id, prefix + "data/",
            [&](std::size_t n) { return prefix + "data/" + pd::digits(n, 6) + ".json"; });
        b.objects = std::move(got.objects);
        b.items = got.items;
        b.end_ms = std::max(_backend.now_ms(), b.start_ms);
        _store->put(prefix + "base.json", pd::encode_base(b));
        return b;
    }

    /// Drops bases wholly before the window (bar the newest of them) and
    /// the log objects only they needed.
    void prune(const table_descriptor& d, const std::string& archive, std::vector<pitr_base>& bases,
               std::uint64_t now) {
        namespace pd = pitr_detail;
        const auto window = static_cast<std::uint64_t>(d.pitr.recovery_days) * pd::k_day_ms;
        const auto window_start = now > window ? now - window : 0;
        std::size_t keep_from = 0;
        for (std::size_t i = 0; i + 1 < bases.size(); ++i) {
            if (bases[i + 1].end_ms <= window_start) {
                keep_from = i + 1;
            }
        }
        if (keep_from == 0) {
            return;
        }
        for (std::size_t i = 0; i < keep_from; ++i) {
            for (const auto& key : _store->list(pd::base_prefix(archive, bases[i].start_ms))) {
                _store->remove(key);
            }
        }
        bases.erase(bases.begin(), bases.begin() + static_cast<std::ptrdiff_t>(keep_from));
        for (const auto& [n, key] : pd::log_objects(*_store, archive)) {
            if (n >= bases.front().log_from) {
                break;
            }
            _store->remove(key);
        }
    }

    /// Removes archives that no table's enabled recovery owns: a disabled
    /// or re-enabled recovery's, or a deleted table's.
    void sweep(const std::vector<table_descriptor>& tables) {
        std::set<std::pair<std::uint64_t, std::uint64_t>> live;
        for (const auto& d : tables) {
            if (d.pitr.enabled) {
                live.emplace(d.table_id, d.pitr.enabled_ms);
            }
        }
        for (const auto& key : _store->list(std::string(pitr_detail::k_prefix))) {
            const auto owner = pitr_detail::archive_of(key);
            if (owner && !live.contains(*owner)) {
                _store->remove(key);
            }
        }
    }

    /// Loads a point-in-time restore: the newest base that ended by the
    /// point, then the archived records after it up to the point.
    void load(const table_descriptor& d) {
        namespace bd = backup_detail;
        const auto put_line = [&](const boost::json::object& image) {
            auto parsed = item_from_json(image);
            if (!parsed) {
                throw std::runtime_error("the archive holds an unreadable item");
            }
            bd::restore_put(_backend, d, std::move(*parsed));
        };
        pitr_detail::replay(*_store, d.restore.source_table_id, d.restore.point_ms,
                            d.restore.point_ms, put_line, [&](const boost::json::object& rec) {
                                if (const auto* image = rec.if_contains("NewImage")) {
                                    put_line(image->as_object());
                                } else {
                                    remove(d, rec.at("Keys").as_object());
                                }
                            });
        bd::finish_restore(_backend, d);
    }

    void remove(const table_descriptor& d, const boost::json::object& keys) {
        auto parsed = item_from_json(keys);
        if (!parsed) {
            throw std::runtime_error("the archive holds an unreadable key");
        }
        item_command c;
        c.op = item_command::kind::delete_;
        c.table_id = d.table_id;
        c.schema = d.schema;
        c.payload = std::move(*parsed);
        c.lsis = d.lsis;
        c.gsis = maintained_indexes(d.gsis);
        c.log_changes = d.logs_changes();
        c.stream_ms = d.stream_label();
        if (_backend.write(c).outcome != item_result::status::ok) {
            throw std::runtime_error("a restored delete was refused by its group");
        }
    }

    Backend& _backend;
    std::shared_ptr<backup_store> _store;
    pitr_worker_options _options;
    std::atomic<bool> _poked{true};
    std::chrono::steady_clock::time_point _next_sweep{};
};

}  // namespace kythira::dynamodb
