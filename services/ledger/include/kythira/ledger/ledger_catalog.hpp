// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The ledger catalog (.kiro/specs/verifiable-ledger/ task 7, Requirements
// 2.3 and 2.4): the names, ids, state and deletion protection of every
// ledger, kept in one Raft group of its own so that creating and deleting
// a ledger are linearizable.
//
// Entries (Ion structs; the leader fills in ids and times, so apply is
// deterministic):
//
//   {type: catalog_create, name, ledgerId, deletionProtection, time}
//   {type: catalog_update, name, deletionProtection}
//   {type: catalog_delete, name, time}
//   {type: catalog_purge, name}
//
// A ledger is active, then deleted (refused while deletion protection is
// on; its journal is kept), then purged by a separate admin operation that
// the node follows by dropping the ledger's group. A deleted or purged
// name stays taken until purged, so a new ledger never inherits an old
// one's name while its journal still exists. Results are Ion: the ledger's
// record, or {status: rejected, code, message} with codes invalid_entry,
// ledger_exists, ledger_not_found, ledger_active, ledger_deleted and
// deletion_protected.
//
// Engine layout under the range's start P:
//   meta  P meta/applied        applied log index (decimal)
//   data  P ledger/<name>       {ledgerId, state, deletionProtection, created, deleted?}

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_types.hpp>

#include <raft/persistent_state_machine.hpp>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::ledger {

/// 1-32 letters, digits and hyphens, starting with a letter (QLDB's rule).
inline auto valid_ledger_name(std::string_view name) -> bool {
    if (name.empty() || name.size() > 32 ||
        std::isalpha(static_cast<unsigned char>(name[0])) == 0) {
        return false;
    }
    return std::ranges::all_of(
        name, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-'; });
}

/// A catalog record.
struct ledger_record {
    std::string name;
    std::string ledger_id;
    std::string state = "active";  // active, deleted, purged
    bool deletion_protection = true;
    std::int64_t created = 0;
    std::optional<std::int64_t> deleted;

    [[nodiscard]] auto to_ion() const -> ion_value {
        ion_value v = ion_value::make_struct();
        v.set("name", ion_value::string(name))
            .set("ledgerId", ion_value::string(ledger_id))
            .set("state", ion_value::symbol(state))
            .set("deletionProtection", ion_value::boolean(deletion_protection))
            .set("created", timestamp_value(created));
        if (deleted) {
            v.set("deleted", timestamp_value(*deleted));
        }
        return v;
    }

    static auto from_ion(const ion_value& v) -> ledger_record {
        namespace jd = journal_detail;
        ledger_record r;
        r.name = jd::text_field(v, "name");
        r.ledger_id = jd::text_field(v, "ledgerId");
        r.state = jd::text_field(v, "state");
        r.deletion_protection = jd::required(v, "deletionProtection").as_bool().value_or(true);
        r.created = jd::time_field(v, "created");
        if (v.find("deleted") != nullptr) {
            r.deleted = jd::time_field(v, "deleted");
        }
        return r;
    }
};

// ── Entries ─────────────────────────────────────────────────────────────────

namespace catalog_entries {

inline auto encode(const ion_value& v) -> std::vector<std::byte> {
    const auto bytes = v.to_binary();
    std::vector<std::byte> out(bytes.size());
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

inline auto create(std::string_view name, std::string_view ledger_id, bool deletion_protection,
                   std::int64_t time) -> std::vector<std::byte> {
    ion_value v = ion_value::make_struct();
    v.set("type", ion_value::symbol("catalog_create"))
        .set("name", ion_value::string(name))
        .set("ledgerId", ion_value::string(ledger_id))
        .set("deletionProtection", ion_value::boolean(deletion_protection))
        .set("time", timestamp_value(time));
    return encode(v);
}

inline auto update(std::string_view name, bool deletion_protection) -> std::vector<std::byte> {
    ion_value v = ion_value::make_struct();
    v.set("type", ion_value::symbol("catalog_update"))
        .set("name", ion_value::string(name))
        .set("deletionProtection", ion_value::boolean(deletion_protection));
    return encode(v);
}

inline auto remove(std::string_view name, std::int64_t time) -> std::vector<std::byte> {
    ion_value v = ion_value::make_struct();
    v.set("type", ion_value::symbol("catalog_delete"))
        .set("name", ion_value::string(name))
        .set("time", timestamp_value(time));
    return encode(v);
}

inline auto purge(std::string_view name) -> std::vector<std::byte> {
    ion_value v = ion_value::make_struct();
    v.set("type", ion_value::symbol("catalog_purge")).set("name", ion_value::string(name));
    return encode(v);
}

}  // namespace catalog_entries

/// A catalog result: the record, or a rejection.
struct catalog_result {
    std::optional<ledger_record> record;
    std::string code;  // empty on success
    std::string message;

    [[nodiscard]] auto ok() const -> bool { return code.empty(); }

    static auto decode(const std::vector<std::byte>& bytes) -> catalog_result {
        const ion_value v = ion_value::parse(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
        catalog_result r;
        if (const ion_value* s = v.find("status");
            s != nullptr && s->as_text().value_or("") == "rejected") {
            r.code = journal_detail::text_field(v, "code");
            r.message = journal_detail::text_field(v, "message");
            return r;
        }
        r.record = ledger_record::from_ion(v);
        return r;
    }
};

class ledger_catalog_error : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

template<kythira::kv_engine Engine> class basic_ledger_catalog {
public:
    using engine_type = Engine;

    auto open(engine_type& engine, const kythira::key_range& range) -> void {
        const std::lock_guard lock(mutex_);
        engine_ = &engine;
        range_ = range;
        prefix_ = range.start;
        load_locked();
    }

    auto apply(const std::vector<std::byte>& command, std::uint64_t index)
        -> std::vector<std::byte> {
        const std::lock_guard lock(mutex_);
        if (index <= applied_) {
            return {};
        }
        kythira::write_batch batch;
        std::vector<std::byte> result;
        try {
            const ion_value v = ion_value::parse(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(command.data()), command.size()));
            result = apply_entry(v, batch);
        } catch (const ledger_format_error& e) {
            batch = {};
            result = rejected("invalid_entry", e.what());
        } catch (const ion_value_error& e) {
            batch = {};
            result = rejected("invalid_entry", e.what());
        }
        batch.put(kythira::column_family::meta, key_applied(), std::to_string(index));
        const auto seq = engine_->write(batch);
        applied_ = index;
        seqs_.emplace_back(index, seq);
        prune_locked();
        return result;
    }

    [[nodiscard]] auto applied_index() const -> std::uint64_t {
        const std::lock_guard lock(mutex_);
        return applied_;
    }

    [[nodiscard]] auto durable_applied_index() const -> std::uint64_t {
        const std::lock_guard lock(mutex_);
        const auto durable = engine_->durable_sequence();
        auto best = durable_floor_;
        for (const auto& [index, seq] : seqs_) {
            if (seq <= durable) {
                best = index;
            }
        }
        return best;
    }

    auto checkpoint(const std::filesystem::path& dir) -> kythira::snapshot_manifest {
        const std::lock_guard lock(mutex_);
        return engine_->checkpoint(range_, dir);
    }

    auto ingest(const std::filesystem::path& dir, const kythira::snapshot_manifest& manifest,
                std::uint64_t index) -> void {
        const std::lock_guard lock(mutex_);
        engine_->ingest(range_, dir, manifest);
        const auto seq = engine_->write(kythira::write_batch{}.put(
            kythira::column_family::meta, key_applied(), std::to_string(index)));
        load_locked();
        seqs_.emplace_back(index, seq);
    }

    auto drop() -> void {
        const std::lock_guard lock(mutex_);
        engine_->drop(range_);
        load_locked();
    }

    // The catalog is one small group: it never splits or merges.
    auto split_at(const std::vector<std::string>& /*keys*/) -> std::vector<kythira::key_range> {
        throw ledger_catalog_error("the ledger catalog never splits");
    }
    auto adopt(const kythira::key_range& /*range*/) -> void {
        throw ledger_catalog_error("the ledger catalog never merges");
    }

    // ── Reads ───────────────────────────────────────────────────────────────

    [[nodiscard]] auto get(std::string_view name) const -> std::optional<ledger_record> {
        const std::lock_guard lock(mutex_);
        return stored(name);
    }

    /// Every ledger, purged ones excluded, by name.
    [[nodiscard]] auto list() const -> std::vector<ledger_record> {
        const std::lock_guard lock(mutex_);
        std::vector<ledger_record> out;
        const std::string from = prefix_ + "ledger/";
        std::string end = from;
        end.back() = static_cast<char>(end.back() + 1);
        engine_->scan(kythira::column_family::data, {from, end}, kythira::scan_direction::forward,
                      [&](std::string_view, std::string_view value) {
                          auto r = ledger_record::from_ion(parse_stored(value));
                          if (r.state != "purged") {
                              out.push_back(std::move(r));
                          }
                          return true;
                      });
        return out;
    }

private:
    auto apply_entry(const ion_value& v, kythira::write_batch& batch) -> std::vector<std::byte> {
        namespace jd = journal_detail;
        const auto type = jd::text_field(v, "type");
        const auto name = jd::text_field(v, "name");
        if (!valid_ledger_name(name)) {
            throw ledger_format_error("invalid ledger name");
        }
        auto current = stored(name);
        if (type == "catalog_create") {
            if (current && current->state != "purged") {
                return rejected("ledger_exists", "ledger " + name + " exists");
            }
            ledger_record r;
            r.name = name;
            r.ledger_id = jd::text_field(v, "ledgerId");
            if (!valid_id(r.ledger_id)) {
                throw ledger_format_error("invalid ledger id");
            }
            r.deletion_protection = jd::required(v, "deletionProtection").as_bool().value_or(true);
            r.created = jd::time_field(v, "time");
            return put(r, batch);
        }
        if (!current || current->state == "purged") {
            return rejected("ledger_not_found", "no ledger " + name);
        }
        if (type == "catalog_update") {
            if (current->state != "active") {
                return rejected("ledger_deleted", "ledger " + name + " is deleted");
            }
            current->deletion_protection =
                jd::required(v, "deletionProtection").as_bool().value_or(true);
            return put(*current, batch);
        }
        if (type == "catalog_delete") {
            if (current->state != "active") {
                return rejected("ledger_deleted", "ledger " + name + " is deleted");
            }
            if (current->deletion_protection) {
                return rejected("deletion_protected",
                                "ledger " + name + " has deletion protection on");
            }
            current->state = "deleted";
            current->deleted = jd::time_field(v, "time");
            return put(*current, batch);
        }
        if (type == "catalog_purge") {
            if (current->state != "deleted") {
                return rejected("ledger_active", "ledger " + name + " must be deleted first");
            }
            current->state = "purged";
            return put(*current, batch);
        }
        throw ledger_format_error("unknown catalog entry type " + type);
    }

    auto put(const ledger_record& r, kythira::write_batch& batch) -> std::vector<std::byte> {
        const auto bytes = r.to_ion().to_binary();
        batch.put(kythira::column_family::data, key_ledger(r.name),
                  std::string(bytes.begin(), bytes.end()));
        return catalog_entries::encode(r.to_ion());
    }

    static auto rejected(std::string_view code, std::string_view message)
        -> std::vector<std::byte> {
        ion_value v = ion_value::make_struct();
        v.set("status", ion_value::symbol("rejected"))
            .set("code", ion_value::symbol(code))
            .set("message", ion_value::string(message));
        return catalog_entries::encode(v);
    }

    static auto parse_stored(std::string_view value) -> ion_value {
        return ion_value::parse(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
    }

    [[nodiscard]] auto stored(std::string_view name) const -> std::optional<ledger_record> {
        const auto v = engine_->get(kythira::column_family::data, key_ledger(name));
        if (!v) {
            return std::nullopt;
        }
        return ledger_record::from_ion(parse_stored(*v));
    }

    [[nodiscard]] auto key_applied() const -> std::string { return prefix_ + "meta/applied"; }
    [[nodiscard]] auto key_ledger(std::string_view name) const -> std::string {
        return prefix_ + "ledger/" + std::string(name);
    }

    auto load_locked() -> void {
        const auto applied = engine_->get(kythira::column_family::meta, key_applied());
        applied_ = applied ? std::stoull(*applied) : 0U;
        seqs_.clear();
        durable_floor_ = applied_;
    }

    auto prune_locked() -> void {
        const auto durable = engine_->durable_sequence();
        std::size_t n = 0;
        while (n < seqs_.size() && seqs_[n].second <= durable) {
            durable_floor_ = seqs_[n].first;
            ++n;
        }
        seqs_.erase(seqs_.begin(), seqs_.begin() + static_cast<std::ptrdiff_t>(n));
    }

    mutable std::mutex mutex_;
    engine_type* engine_ = nullptr;
    kythira::key_range range_;
    std::string prefix_;
    std::uint64_t applied_ = 0;
    std::uint64_t durable_floor_ = 0;
    std::vector<std::pair<std::uint64_t, kythira::engine_sequence>> seqs_;
};

/// The catalog group's key range.
inline auto catalog_range() -> kythira::key_range {
    return {"c/catalog/", "c/catalog0"};
}

}  // namespace kythira::ledger
