// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file backup.hpp
/// @brief On-demand backups: `CreateBackup`, `DescribeBackup`, `ListBackups`,
///        `DeleteBackup` and `RestoreTableFromBackup`
///        (`.kiro/specs/dynamodb-compatible-api/` task 18, Requirement 17.1).
///
/// A backup lives wholly in an object store (`backup_store`): any Kythira
/// object store through `object_backup_store`, a directory, or memory for a
/// single in-memory node. Under the store's prefix each backup is
///
///     backups/<id>/backup.json        the record: name, ARN, status, the
///                                     source table's descriptor, counts,
///                                     and every data object with its
///                                     SHA-256
///     backups/<id>/data/<n>.json      the items, one DynamoDB JSON
///                                     `{"Item":{...}}` per line, as AWS's
///                                     DYNAMODB_JSON export writes them
///
/// so the record is the manifest, and the catalog keeps no backup state:
/// listing backups lists records. `<id>` is AWS's form, 14 digits of
/// milliseconds, a dash and 8 hex digits; the ARN is the table's with
/// `/backup/<id>`.
///
/// `CreateBackup` writes a CREATING record and answers. `backup_worker`, on
/// the catalog leader, takes each CREATING record, reads the table with
/// consistent range reads (each item group at its leader after a read
/// barrier), writes the data objects and rewrites the record AVAILABLE. A
/// worker that loses leadership mid-backup leaves the record CREATING and
/// the next leader writes it again from the start; objects are rewritten
/// whole, so a rerun is idempotent. If the table is gone by then, the
/// backup is deleted.
///
/// Deviation from the design: there is no one-applied-index-per-group
/// checkpoint. Every write acknowledged before `CreateBackup` is in the
/// backup (the reads come after it, linearizably), and a write made while
/// the backup runs may or may not be, which is the consistency AWS states
/// for on-demand backups. Index entries are not stored: a restore writes
/// every item through the ordinary write path, which rebuilds the LSIs and
/// GSIs it keeps.
///
/// `RestoreTableFromBackup` creates the target table CREATING, with the
/// source's key schema, indexes (or the subset an index override names)
/// and capacity (or the overrides), and a `RestoreSummary` with
/// `RestoreInProgress`. Streams, TTL, tags, Kinesis destinations and
/// deletion protection are not restored, as on AWS. The worker loads the
/// items, then `restore_done` lets the backend make the table ACTIVE; a new
/// catalog leader loads them again (puts are idempotent).
///
/// DynamoDB Local has no backups, so the refusals follow AWS in kind
/// (`TableNotFoundException`, `TableInUseException`,
/// `BackupNotFoundException`, `BackupInUseException`,
/// `TableAlreadyExistsException`) in Kythira's words.

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/global_index.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/key_layout.hpp>
#include <kythira/dynamodb/table_api.hpp>

#include <raft/aws_json_protocol.hpp>
#include <raft/key_object_store.hpp>
#include <raft/sigv4_verifier.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

// -- Stores -------------------------------------------------------------------

/// Where backups live: a flat key to bytes store. Keys are relative paths
/// (`backups/<id>/backup.json`).
class backup_store {
public:
    backup_store() = default;
    backup_store(const backup_store&) = delete;
    backup_store(backup_store&&) = delete;
    auto operator=(const backup_store&) -> backup_store& = delete;
    auto operator=(backup_store&&) -> backup_store& = delete;
    virtual ~backup_store() = default;

    virtual void put(const std::string& key, std::string_view bytes) = 0;
    [[nodiscard]] virtual auto get(const std::string& key) -> std::optional<std::string> = 0;
    virtual void remove(const std::string& key) = 0;
    /// Every key starting with `prefix`, in any order.
    [[nodiscard]] virtual auto list(const std::string& prefix) -> std::vector<std::string> = 0;
};

/// Backups in memory: a single node on the memory engine, whose items do not
/// outlive the process either, and tests.
class memory_backup_store final : public backup_store {
public:
    void put(const std::string& key, std::string_view bytes) override {
        const std::lock_guard lock(_mu);
        _objects[key] = std::string(bytes);
    }
    [[nodiscard]] auto get(const std::string& key) -> std::optional<std::string> override {
        const std::lock_guard lock(_mu);
        const auto it = _objects.find(key);
        return it == _objects.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    void remove(const std::string& key) override {
        const std::lock_guard lock(_mu);
        _objects.erase(key);
    }
    [[nodiscard]] auto list(const std::string& prefix) -> std::vector<std::string> override {
        const std::lock_guard lock(_mu);
        std::vector<std::string> out;
        for (auto it = _objects.lower_bound(prefix);
             it != _objects.end() && it->first.starts_with(prefix); ++it) {
            out.push_back(it->first);
        }
        return out;
    }

private:
    std::mutex _mu;
    std::map<std::string, std::string> _objects;
};

/// Backups as files under a directory: one node's disk, or a shared mount
/// every node of a cluster sees. Writes go to a temporary file renamed into
/// place, so a reader never sees half an object.
class directory_backup_store final : public backup_store {
public:
    explicit directory_backup_store(std::filesystem::path root) : _root(std::move(root)) {
        std::filesystem::create_directories(_root);
    }

    void put(const std::string& key, std::string_view bytes) override {
        const auto path = path_of(key);
        std::filesystem::create_directories(path.parent_path());
        auto tmp = path;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            out.flush();
            if (!out) {
                throw std::runtime_error("directory_backup_store: cannot write " + tmp.string());
            }
        }
        std::filesystem::rename(tmp, path);
    }
    [[nodiscard]] auto get(const std::string& key) -> std::optional<std::string> override {
        std::ifstream in(path_of(key), std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        return buf.str();
    }
    void remove(const std::string& key) override {
        std::error_code ec;
        std::filesystem::remove(path_of(key), ec);
    }
    [[nodiscard]] auto list(const std::string& prefix) -> std::vector<std::string> override {
        std::vector<std::string> out;
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(_root, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file()) {
                continue;
            }
            auto key = std::filesystem::relative(it->path(), _root).generic_string();
            if (key.ends_with(".tmp")) {
                continue;
            }
            if (key.starts_with(prefix)) {
                out.push_back(std::move(key));
            }
        }
        return out;
    }

private:
    [[nodiscard]] auto path_of(const std::string& key) const -> std::filesystem::path {
        // Keys are made here (backups/<id>/...), never from a client's
        // text; refuse anything that could leave the root anyway.
        if (key.empty() || key.front() == '/' || key.find("..") != std::string::npos) {
            throw std::invalid_argument("directory_backup_store: bad key " + key);
        }
        return _root / key;
    }

    std::filesystem::path _root;
};

/// Backups in any Kythira object store (`key_object_store`: S3, Azure Blob,
/// GCS, OCI Object Storage, Alibaba OSS) under `bucket` and `prefix`.
template<kythira::key_object_store Store> class object_backup_store final : public backup_store {
public:
    object_backup_store(std::shared_ptr<Store> store, std::string bucket, std::string prefix = {})
        : _store(std::move(store)), _bucket(std::move(bucket)), _prefix(std::move(prefix)) {}

    void put(const std::string& key, std::string_view bytes) override {
        (void)_store->put_object(_bucket, _prefix + key, std::string(bytes));
    }
    [[nodiscard]] auto get(const std::string& key) -> std::optional<std::string> override {
        auto got = _store->get_object(_bucket, _prefix + key);
        if (!got) {
            return std::nullopt;
        }
        return std::move(got->body);
    }
    void remove(const std::string& key) override { _store->delete_object(_bucket, _prefix + key); }
    [[nodiscard]] auto list(const std::string& prefix) -> std::vector<std::string> override {
        auto keys = _store->list_keys(_bucket, _prefix + prefix);
        for (auto& k : keys) {
            k = k.substr(std::min(k.size(), _prefix.size()));
        }
        return keys;
    }

private:
    std::shared_ptr<Store> _store;
    std::string _bucket;
    std::string _prefix;
};

// -- Records ------------------------------------------------------------------

/// One data object of a backup.
struct backup_object {
    std::string key;
    std::uint64_t items = 0;
    std::uint64_t bytes = 0;
    std::string sha256;

    friend auto operator==(const backup_object&, const backup_object&) -> bool = default;
};

/// A backup's record (`backup.json`).
struct backup_record {
    std::string id;
    std::string arn;
    std::string name;
    std::string table_name;
    std::string table_arn;
    /// The source table's descriptor when the backup was asked for.
    table_descriptor source;
    /// CREATING or AVAILABLE.
    std::string status = "CREATING";
    std::uint64_t created_ms = 0;
    /// The items' DynamoDB sizes, summed.
    std::uint64_t size_bytes = 0;
    std::uint64_t item_count = 0;
    std::vector<backup_object> objects;

    friend auto operator==(const backup_record&, const backup_record&) -> bool = default;
};

namespace backup_detail {

inline constexpr std::string_view k_backups_prefix = "backups/";
/// Items per data object, and about how many bytes.
inline constexpr std::size_t k_object_items = 10000;
inline constexpr std::size_t k_object_bytes = std::size_t{16} << 20U;

[[nodiscard]] inline auto record_key(std::string_view id) -> std::string {
    return std::string(k_backups_prefix) + std::string(id) + "/backup.json";
}
[[nodiscard]] inline auto data_key(std::string_view id, std::size_t n) -> std::string {
    std::array<char, 16> buf{};
    const auto len = std::snprintf(buf.data(), buf.size(), "%06zu", n);
    return std::string(k_backups_prefix) + std::string(id) + "/data/" +
           std::string(buf.data(), static_cast<std::size_t>(len)) + ".json";
}

/// AWS's backup id: 14 digits of milliseconds, a dash, 8 hex digits.
[[nodiscard]] inline auto new_backup_id(std::uint64_t now_ms) -> std::string {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::array<char, 32> buf{};
    const auto n = std::snprintf(buf.data(), buf.size(), "%014llu-%08llx",
                                 static_cast<unsigned long long>(now_ms),
                                 static_cast<unsigned long long>(rng() & 0xFFFFFFFFULL));
    return {buf.data(), static_cast<std::size_t>(n)};
}

/// The id at the end of a backup ARN
/// (`arn:...:table/<name>/backup/<id>`), or nothing.
[[nodiscard]] inline auto id_of_arn(std::string_view arn) -> std::optional<std::string> {
    const auto at = arn.rfind("/backup/");
    if (at == std::string_view::npos || !arn.starts_with("arn:")) {
        return std::nullopt;
    }
    const auto id = arn.substr(at + 8);
    if (id.size() != 23 || id[14] != '-' ||
        !std::all_of(id.begin(), id.begin() + 14, [](char c) { return c >= '0' && c <= '9'; }) ||
        !std::all_of(id.begin() + 15, id.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) {
        return std::nullopt;
    }
    return std::string(id);
}

[[nodiscard]] inline auto valid_backup_name(std::string_view name) -> bool {
    return name.size() >= 3 && name.size() <= 255 &&
           std::all_of(name.begin(), name.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-' || c == '.';
           });
}

[[nodiscard]] inline auto encode_record(const backup_record& r) -> std::string {
    boost::json::array objects;
    for (const auto& o : r.objects) {
        objects.push_back(boost::json::object{
            {"key", o.key}, {"items", o.items}, {"bytes", o.bytes}, {"sha256", o.sha256}});
    }
    return boost::json::serialize(boost::json::object{
        {"formatVersion", 1},
        {"id", r.id},
        {"arn", r.arn},
        {"name", r.name},
        {"tableName", r.table_name},
        {"tableArn", r.table_arn},
        {"source", detail::base64_of(encode_table_descriptor(r.source))},
        {"status", r.status},
        {"createdMs", r.created_ms},
        {"sizeBytes", r.size_bytes},
        {"itemCount", r.item_count},
        {"objects", std::move(objects)},
    });
}

[[nodiscard]] inline auto decode_record(std::string_view text) -> std::optional<backup_record> {
    try {
        const auto v = boost::json::parse(text);
        const auto& o = v.as_object();
        if (o.at("formatVersion").to_number<int>() != 1) {
            return std::nullopt;
        }
        backup_record r;
        r.id = std::string(o.at("id").as_string());
        r.arn = std::string(o.at("arn").as_string());
        r.name = std::string(o.at("name").as_string());
        r.table_name = std::string(o.at("tableName").as_string());
        r.table_arn = std::string(o.at("tableArn").as_string());
        const auto raw = aws_json::base64_decode(o.at("source").as_string());
        if (!raw) {
            return std::nullopt;
        }
        auto source = decode_table_descriptor(
            std::string_view(reinterpret_cast<const char*>(raw->data()), raw->size()));  // NOLINT
        if (!source) {
            return std::nullopt;
        }
        r.source = std::move(*source);
        r.status = std::string(o.at("status").as_string());
        r.created_ms = o.at("createdMs").to_number<std::uint64_t>();
        r.size_bytes = o.at("sizeBytes").to_number<std::uint64_t>();
        r.item_count = o.at("itemCount").to_number<std::uint64_t>();
        for (const auto& e : o.at("objects").as_array()) {
            const auto& obj = e.as_object();
            r.objects.push_back(backup_object{std::string(obj.at("key").as_string()),
                                              obj.at("items").to_number<std::uint64_t>(),
                                              obj.at("bytes").to_number<std::uint64_t>(),
                                              std::string(obj.at("sha256").as_string())});
        }
        return r;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// Every backup record in `store`, oldest first.
[[nodiscard]] inline auto all_records(backup_store& store) -> std::vector<backup_record> {
    std::vector<backup_record> out;
    for (const auto& key : store.list(std::string(k_backups_prefix))) {
        if (!key.ends_with("/backup.json")) {
            continue;
        }
        if (const auto text = store.get(key)) {
            if (auto r = decode_record(*text)) {
                out.push_back(std::move(*r));
            }
        }
    }
    std::ranges::sort(out, [](const backup_record& a, const backup_record& b) {
        return a.created_ms != b.created_ms ? a.created_ms < b.created_ms : a.id < b.id;
    });
    return out;
}

[[nodiscard]] inline auto modeled(std::string_view type, std::string message) -> aws_json::error {
    return table_api_detail::modeled(type, std::move(message));
}

[[nodiscard]] inline auto backup_not_found(std::string_view arn) -> aws_json::error {
    return modeled("BackupNotFoundException", "Backup not found: " + std::string(arn));
}

[[nodiscard]] inline auto not_configured() -> aws_json::error {
    return validation_error(
        "Backups are not configured on this cluster: set KYTHIRA_DYNAMODB_BACKUP_DIR or "
        "KYTHIRA_DYNAMODB_BACKUP_S3_BUCKET");
}

/// The source table's settings a restore keeps, with `name` and nothing a
/// restore drops (streams, TTL, tags, destinations, deletion protection,
/// point-in-time recovery).
[[nodiscard]] inline auto restored_descriptor(const table_descriptor& source,
                                              const std::string& name) -> table_descriptor {
    table_descriptor d;
    d.name = name;
    d.schema = source.schema;
    d.billing = source.billing;
    d.read_capacity = source.read_capacity;
    d.write_capacity = source.write_capacity;
    d.storage_class = source.storage_class;
    d.warm_read = source.warm_read;
    d.warm_write = source.warm_write;
    d.lsis = source.lsis;
    for (const auto& g : maintained_indexes(source.gsis)) {
        d.gsis.push_back(g);
    }
    return d;
}

[[nodiscard]] inline auto text(const boost::json::object& in, std::string_view key) -> std::string {
    const auto* v = in.if_contains(key);
    return v != nullptr && v->is_string() ? std::string(v->as_string()) : std::string{};
}

/// `BillingModeOverride`, `ProvisionedThroughputOverride`, and index
/// overrides naming which of the backup's indexes to keep.
inline auto apply_overrides(const boost::json::object& in, table_descriptor& d)
    -> std::optional<aws_json::error> {
    const auto billing = text(in, "BillingModeOverride");
    if (billing == "PAY_PER_REQUEST") {
        d.billing = billing_mode::pay_per_request;
        d.read_capacity = 0;
        d.write_capacity = 0;
        for (auto& g : d.gsis) {
            g.read_capacity = 0;
            g.write_capacity = 0;
        }
    } else if (billing == "PROVISIONED") {
        d.billing = billing_mode::provisioned;
    }
    if (const auto* p = in.if_contains("ProvisionedThroughputOverride");
        p != nullptr && p->is_object()) {
        if (d.billing == billing_mode::pay_per_request) {
            return validation_error(
                "ProvisionedThroughput cannot be specified when BillingMode is "
                "PAY_PER_REQUEST");
        }
        d.read_capacity = p->as_object().at("ReadCapacityUnits").to_number<std::uint64_t>();
        d.write_capacity = p->as_object().at("WriteCapacityUnits").to_number<std::uint64_t>();
    }
    if (d.billing == billing_mode::provisioned && (d.read_capacity == 0 || d.write_capacity == 0)) {
        return validation_error(
            "ProvisionedThroughputOverride is required when restoring an on-demand table "
            "as PROVISIONED");
    }
    const auto names = [&](std::string_view key) -> std::optional<std::vector<std::string>> {
        const auto* v = in.if_contains(key);
        if (v == nullptr || !v->is_array()) {
            return std::nullopt;
        }
        std::vector<std::string> out;
        for (const auto& e : v->as_array()) {
            out.emplace_back(e.as_object().at("IndexName").as_string());
        }
        return out;
    };
    if (const auto keep = names("GlobalSecondaryIndexOverride")) {
        for (const auto& n : *keep) {
            if (std::ranges::none_of(d.gsis, [&](const global_index& g) { return g.name == n; })) {
                return validation_error("GlobalSecondaryIndexOverride names " + n +
                                        ", which the source does not have");
            }
        }
        std::erase_if(d.gsis, [&](const global_index& g) {
            return std::ranges::find(*keep, g.name) == keep->end();
        });
    }
    if (const auto keep = names("LocalSecondaryIndexOverride")) {
        for (const auto& n : *keep) {
            if (std::ranges::none_of(d.lsis, [&](const local_index& l) { return l.name == n; })) {
                return validation_error("LocalSecondaryIndexOverride names " + n +
                                        ", which the source does not have");
            }
        }
        std::erase_if(d.lsis, [&](const local_index& l) {
            return std::ranges::find(*keep, l.name) == keep->end();
        });
    }
    // Unlike the other index overrides, a vector index override gives the
    // whole definitions: the restored table has exactly these.
    if (const auto* v = in.if_contains("VectorIndexOverride"); v != nullptr && v->is_array()) {
        std::vector<vector_index> vectors;
        for (const auto& e : v->as_array()) {
            auto parsed = vector_index_from(e.as_object(), d.schema);
            if (!parsed) {
                return parsed.error();
            }
            if (std::ranges::any_of(
                    vectors, [&](const vector_index& x) { return x.name == parsed->name; })) {
                return validation_error("Duplicate index name: " + parsed->name);
            }
            vectors.push_back(std::move(*parsed));
        }
        d.vectors = std::move(vectors);
    }
    return std::nullopt;
}

/// What `write_items` wrote.
struct written_items {
    std::vector<backup_object> objects;
    std::uint64_t items = 0;
    /// The items' DynamoDB sizes, summed.
    std::uint64_t size_bytes = 0;
};

/// Writes every item of `table_id` to `store` as DynamoDB JSON lines, with
/// a consistent read of the table's whole range, in objects named by
/// `key_of(n)`. Objects left from an earlier, longer attempt under
/// `prefix` are removed.
template<typename Backend, typename KeyOf>
auto write_items(Backend& backend, backup_store& store, std::uint64_t table_id,
                 const std::string& prefix, KeyOf key_of) -> written_items {
    written_items out;
    std::string chunk;
    std::uint64_t chunk_items = 0;
    const auto flush = [&] {
        if (chunk_items == 0) {
            return;
        }
        backup_object o;
        o.key = key_of(out.objects.size());
        o.items = chunk_items;
        o.bytes = chunk.size();
        o.sha256 = sigv4::detail::sha256_hex(chunk);
        store.put(o.key, chunk);
        out.objects.push_back(std::move(o));
        chunk.clear();
        chunk_items = 0;
    };
    backend.read_range(
        table_range(table_id), kythira::scan_direction::forward, true,
        [&](std::string_view, const item& it) {
            chunk += boost::json::serialize(boost::json::object{{"Item", item_to_json(it)}});
            chunk += '\n';
            ++chunk_items;
            ++out.items;
            out.size_bytes += item_size(it);
            if (chunk_items >= k_object_items || chunk.size() >= k_object_bytes) {
                flush();
            }
            return true;
        });
    flush();
    for (const auto& key : store.list(prefix)) {
        if (std::ranges::none_of(out.objects,
                                 [&](const backup_object& o) { return o.key == key; })) {
            store.remove(key);
        }
    }
    return out;
}

/// Calls `fn` with each line of the object, after checking its sha256.
template<typename Fn> void read_object(backup_store& store, const backup_object& o, Fn fn) {
    const auto body = store.get(o.key);
    if (!body || sigv4::detail::sha256_hex(*body) != o.sha256) {
        throw std::runtime_error("backup object " + o.key + " is missing or damaged");
    }
    std::string_view rest(*body);
    while (!rest.empty()) {
        const auto nl = rest.find('\n');
        const auto line = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        if (!line.empty()) {
            fn(line);
        }
    }
}

/// Puts `it` into the restoring table `d` through the ordinary write path,
/// so its indexes are built. A put the group refuses stops the load; the
/// next pass reloads, and the puts already made are rewritten to the same
/// values.
template<typename Backend> void restore_put(Backend& backend, const table_descriptor& d, item it) {
    item_command c;
    c.op = item_command::kind::put;
    c.table_id = d.table_id;
    c.schema = d.schema;
    c.payload = std::move(it);
    c.lsis = d.lsis;
    c.gsis = maintained_indexes(d.gsis);
    c.log_changes = d.logs_changes();
    c.stream_ms = d.stream_label();
    c.region = d.region;
    if (backend.write(c).outcome != item_result::status::ok) {
        throw std::runtime_error("a restored item was refused by its group");
    }
}

/// Tells the catalog the restoring table `d` is loaded.
template<typename Backend> void finish_restore(Backend& backend, const table_descriptor& d) {
    catalog_command c;
    c.op = catalog_command::kind::restore_done;
    c.table.name = d.name;
    c.table.table_id = d.table_id;
    c.now_ms = backend.now_ms();
    (void)backend.catalog(c);
}

}  // namespace backup_detail

// -- The API ------------------------------------------------------------------

/// The five backup actions over a backend and a store. Without a store
/// every action but `ListBackups` refuses.
template<dynamo_backend Backend> class backup_api {
public:
    using result = aws_json::result<boost::json::object>;

    backup_api(Backend& backend, std::shared_ptr<backup_store> store)
        : _backend(backend), _store(std::move(store)) {}

    void set_store(std::shared_ptr<backup_store> store) { _store = std::move(store); }
    /// Called when a backup is asked for, so the worker looks at once.
    void on_created(std::function<void()> fn) { _created = std::move(fn); }

    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        fe.on("CreateBackup",
              [this](call_context& c, boost::json::object& in) { return create_backup(c, in); });
        fe.on("DescribeBackup",
              [this](call_context& c, boost::json::object& in) { return describe_backup(c, in); });
        fe.on("ListBackups",
              [this](call_context& c, boost::json::object& in) { return list_backups(c, in); });
        fe.on("DeleteBackup",
              [this](call_context& c, boost::json::object& in) { return delete_backup(c, in); });
        fe.on("RestoreTableFromBackup",
              [this](call_context& c, boost::json::object& in) { return restore(c, in); });
    }

    auto create_backup(call_context& ctx, boost::json::object& in) -> result {
        namespace bd = backup_detail;
        const auto name = ctx.table_name(in.at("TableName").as_string());
        if (!table_api_detail::valid_catalog_name(ctx, name)) {
            return table_api_detail::bad_table_name();
        }
        const std::string backup_name(in.at("BackupName").as_string());
        if (!bd::valid_backup_name(backup_name)) {
            return validation_error(
                "Invalid backup name.  Backup names must be between 3 and 255 characters long, "
                "and may contain only the characters a-z, A-Z, 0-9, '_', '-', and '.'");
        }
        const auto d = _backend.describe(name);
        if (!d) {
            return bd::modeled("TableNotFoundException", "Table not found: " + name);
        }
        if (!_store) {
            return bd::not_configured();
        }
        if (d->status != table_status::active) {
            return bd::modeled("TableInUseException",
                               "Table is being " +
                                   std::string(d->status == table_status::creating   ? "created"
                                               : d->status == table_status::deleting ? "deleted"
                                                                                     : "updated") +
                                   ": " + name);
        }
        backup_record r;
        r.created_ms = _backend.now_ms();
        r.id = bd::new_backup_id(r.created_ms);
        r.table_name = d->name;
        r.table_arn = ctx.table_arn(d->name);
        r.arn = r.table_arn + "/backup/" + r.id;
        r.name = backup_name;
        r.source = *d;
        _store->put(bd::record_key(r.id), bd::encode_record(r));
        if (_created) {
            _created();
        }
        return boost::json::object{{"BackupDetails", details(r)}};
    }

    auto describe_backup(call_context& ctx, boost::json::object& in) -> result {
        const std::string arn(in.at("BackupArn").as_string());
        const auto r = find(arn);
        if (!r) {
            return backup_detail::backup_not_found(arn);
        }
        return boost::json::object{{"BackupDescription", description(*r, ctx)}};
    }

    auto list_backups(call_context& ctx, boost::json::object& in) -> result {
        boost::json::array summaries;
        if (!_store) {
            return boost::json::object{{"BackupSummaries", std::move(summaries)}};
        }
        std::string table;
        if (const auto* t = in.if_contains("TableName"); t != nullptr && t->is_string()) {
            table = ctx.table_name(t->as_string());
        }
        const auto type = text(in, "BackupType");
        if (!type.empty() && type != "USER" && type != "ALL") {
            // Kythira makes only user backups.
            return boost::json::object{{"BackupSummaries", std::move(summaries)}};
        }
        const auto bound = [&](std::string_view key) -> std::optional<double> {
            const auto* v = in.if_contains(key);
            return v != nullptr && v->is_number() ? std::optional<double>(v->to_number<double>())
                                                  : std::nullopt;
        };
        const auto lower = bound("TimeRangeLowerBound");
        const auto upper = bound("TimeRangeUpperBound");
        std::size_t limit = SIZE_MAX;
        if (const auto* l = in.if_contains("Limit"); l != nullptr && l->is_number()) {
            limit = l->to_number<std::size_t>();
        }
        const auto start = text(in, "ExclusiveStartBackupArn");
        bool started = start.empty();
        std::string last;
        bool more = false;
        for (const auto& r : backup_detail::all_records(*_store)) {
            if (!started) {
                started = r.arn == start;
                continue;
            }
            const auto at = table_api_detail::millis_to_seconds(r.created_ms);
            if (!ctx.owns_arn(r.arn) ||
                (!table.empty() &&
                 call_context::bare_name(r.table_name) != call_context::bare_name(table)) ||
                (lower && at < *lower) || (upper && at > *upper)) {
                continue;
            }
            if (summaries.size() >= limit) {
                more = true;
                break;
            }
            auto s = details(r);
            s["TableName"] = call_context::bare_name(r.table_name);
            s["TableId"] = table_api_detail::table_uuid(r.source.table_id);
            s["TableArn"] = r.table_arn;
            summaries.push_back(std::move(s));
            last = r.arn;
        }
        boost::json::object out{{"BackupSummaries", std::move(summaries)}};
        if (more) {
            out["LastEvaluatedBackupArn"] = last;
        }
        return out;
    }

    auto delete_backup(call_context& ctx, boost::json::object& in) -> result {
        const std::string arn(in.at("BackupArn").as_string());
        auto r = find(arn);
        if (!r) {
            return backup_detail::backup_not_found(arn);
        }
        if (r->status == "CREATING") {
            return backup_detail::modeled("BackupInUseException",
                                          "Backup is being created: " + arn);
        }
        for (const auto& o : r->objects) {
            _store->remove(o.key);
        }
        _store->remove(backup_detail::record_key(r->id));
        r->status = "DELETED";
        return boost::json::object{{"BackupDescription", description(*r, ctx)}};
    }

    auto restore(call_context& ctx, boost::json::object& in) -> result {
        namespace bd = backup_detail;
        const std::string target(in.at("TargetTableName").as_string());
        if (!table_api_detail::valid_table_name(target)) {
            return table_api_detail::bad_table_name();
        }
        // The restored table is the caller's: under its account with
        // namespaces on. `target` stays the name the caller gave, for the
        // messages.
        const auto target_table = ctx.table_name(target);
        const std::string arn(in.at("BackupArn").as_string());
        const auto r = find(arn);
        if (!r) {
            return bd::backup_not_found(arn);
        }
        if (r->status != "AVAILABLE") {
            return bd::modeled("BackupInUseException", "Backup is being created: " + arn);
        }
        if (_backend.describe(target_table)) {
            return bd::modeled("TableAlreadyExistsException", "Table already exists: " + target);
        }
        // A restore makes a table, so it counts against the owner's table
        // limits as CreateTable and ImportTable do.
        if (auto e = table_api_detail::within_table_limits(_backend, ctx, true)) {
            return *e;
        }
        auto d = bd::restored_descriptor(r->source, target_table);
        if (auto e = bd::apply_overrides(in, d)) {
            return *e;
        }
        d.restore.source_backup_arn = r->arn;
        d.restore.source_table_arn = r->table_arn;
        catalog_command c;
        c.op = catalog_command::kind::create_table;
        c.table = std::move(d);
        c.now_ms = _backend.now_ms();
        const auto res = _backend.catalog(c);
        if (!res.ok()) {
            if (res.error_type.ends_with("ResourceInUseException")) {
                return bd::modeled("TableAlreadyExistsException",
                                   "Table already exists: " + target);
            }
            return table_api_detail::error_of(res);
        }
        if (_created) {
            _created();
        }
        return boost::json::object{{"TableDescription", table_description(*res.table, ctx)}};
    }

private:
    static auto text(const boost::json::object& in, std::string_view key) -> std::string {
        return backup_detail::text(in, key);
    }

    [[nodiscard]] auto find(const std::string& arn) -> std::optional<backup_record> {
        const auto id = backup_detail::id_of_arn(arn);
        if (!id || !_store) {
            return std::nullopt;
        }
        const auto text = _store->get(backup_detail::record_key(*id));
        if (!text) {
            return std::nullopt;
        }
        auto r = backup_detail::decode_record(*text);
        if (!r || r->arn != arn) {
            return std::nullopt;
        }
        return r;
    }

    static auto details(const backup_record& r) -> boost::json::object {
        return boost::json::object{
            {"BackupArn", r.arn},
            {"BackupName", r.name},
            {"BackupSizeBytes", r.size_bytes},
            {"BackupStatus", r.status},
            {"BackupType", "USER"},
            {"BackupCreationDateTime", table_api_detail::millis_to_seconds(r.created_ms)},
        };
    }

    static auto description(const backup_record& r, const call_context& ctx)
        -> boost::json::object {
        const auto table = table_description(r.source, ctx);
        boost::json::object source{
            {"TableName", call_context::bare_name(r.table_name)},
            {"TableId", table_api_detail::table_uuid(r.source.table_id)},
            {"TableArn", r.table_arn},
            {"TableSizeBytes", r.size_bytes},
            {"KeySchema", table.at("KeySchema")},
            {"TableCreationDateTime", table_api_detail::millis_to_seconds(r.source.created_ms)},
            {"ProvisionedThroughput",
             boost::json::object{{"ReadCapacityUnits", r.source.read_capacity},
                                 {"WriteCapacityUnits", r.source.write_capacity}}},
            {"ItemCount", r.item_count},
            {"BillingMode",
             r.source.billing == billing_mode::pay_per_request ? "PAY_PER_REQUEST" : "PROVISIONED"},
        };
        boost::json::object features;
        const auto pick = [](const boost::json::value& list,
                             std::initializer_list<std::string_view> keep) {
            boost::json::array out;
            for (const auto& e : list.as_array()) {
                boost::json::object o;
                for (const auto k : keep) {
                    if (const auto* v = e.as_object().if_contains(k)) {
                        o[k] = *v;
                    }
                }
                out.push_back(std::move(o));
            }
            return out;
        };
        if (const auto* l = table.if_contains("LocalSecondaryIndexes")) {
            features["LocalSecondaryIndexes"] =
                pick(*l, {"IndexName", "KeySchema", "Projection", "IndexSizeBytes", "ItemCount"});
        }
        if (const auto* g = table.if_contains("GlobalSecondaryIndexes")) {
            features["GlobalSecondaryIndexes"] =
                pick(*g, {"IndexName", "KeySchema", "Projection", "ProvisionedThroughput"});
        }
        if (const auto* s = table.if_contains("StreamSpecification")) {
            features["StreamDescription"] = *s;
        }
        features["TimeToLiveDescription"] = boost::json::object{
            {"TimeToLiveStatus", r.source.ttl.enabled ? "ENABLED" : "DISABLED"}};
        if (r.source.ttl.enabled) {
            features["TimeToLiveDescription"].as_object()["AttributeName"] = r.source.ttl.attribute;
        }
        return boost::json::object{{"BackupDetails", details(r)},
                                   {"SourceTableDetails", std::move(source)},
                                   {"SourceTableFeatureDetails", std::move(features)}};
    }

    Backend& _backend;
    std::shared_ptr<backup_store> _store;
    std::function<void()> _created;
};

// -- The worker ---------------------------------------------------------------

struct backup_worker_options {
    /// How often the store is listed for CREATING backups when nothing was
    /// asked for on this node.
    std::chrono::milliseconds list_interval{10000};
};

/// Takes backups and loads restores, on the catalog leader.
template<dynamo_backend Backend> class backup_worker {
public:
    backup_worker(Backend& backend, std::shared_ptr<backup_store> store,
                  backup_worker_options options = {})
        : _backend(backend), _store(std::move(store)), _options(options) {}

    void set_store(std::shared_ptr<backup_store> store) { _store = std::move(store); }
    /// Look at the store on the next pass, whatever the interval.
    void poke() { _poked.store(true); }

    /// One pass: every CREATING backup taken, every restoring table loaded.
    /// Returns how many of either finished.
    auto pass() -> std::size_t {
        if (!_store) {
            return 0;
        }
        std::size_t done = 0;
        const auto now = std::chrono::steady_clock::now();
        if (_poked.exchange(false) || now >= _next_list) {
            _next_list = now + _options.list_interval;
            for (auto& r : backup_detail::all_records(*_store)) {
                if (r.status == "CREATING") {
                    take(r);
                    ++done;
                }
            }
        }
        for (const auto& name : _backend.list_tables("", SIZE_MAX)) {
            const auto d = _backend.describe(name);
            // A point-in-time restore is the archiver's (`pitr.hpp`), an
            // import the importer's (`export_import.hpp`).
            if (d && d->status == table_status::creating && d->restore.in_progress &&
                !d->restore.source_backup_arn.empty() && !d->restore.imported()) {
                load(*d);
                ++done;
            }
        }
        return done;
    }

private:
    /// Writes the backup's data objects and its AVAILABLE record, or deletes
    /// it when its table is gone.
    void take(backup_record& r) {
        namespace bd = backup_detail;
        const auto d = _backend.describe(r.table_name);
        if (!d || d->table_id != r.source.table_id) {
            for (const auto& key : _store->list(std::string(bd::k_backups_prefix) + r.id + "/")) {
                _store->remove(key);
            }
            return;
        }
        auto got = bd::write_items(_backend, *_store, r.source.table_id,
                                   std::string(bd::k_backups_prefix) + r.id + "/data/",
                                   [&](std::size_t n) { return bd::data_key(r.id, n); });
        r.objects = std::move(got.objects);
        r.size_bytes = got.size_bytes;
        r.item_count = got.items;
        r.status = "AVAILABLE";
        _store->put(bd::record_key(r.id), bd::encode_record(r));
    }

    /// Loads a restoring table's items from its backup, then lets it become
    /// ACTIVE.
    void load(const table_descriptor& d) {
        namespace bd = backup_detail;
        const auto id = bd::id_of_arn(d.restore.source_backup_arn);
        std::optional<backup_record> r;
        if (id) {
            if (const auto text = _store->get(bd::record_key(*id))) {
                r = bd::decode_record(*text);
            }
        }
        if (r) {
            for (const auto& o : r->objects) {
                bd::read_object(*_store, o, [&](std::string_view line) {
                    auto parsed =
                        item_from_json(boost::json::parse(line).as_object().at("Item").as_object());
                    if (!parsed) {
                        throw std::runtime_error("backup holds an unreadable item");
                    }
                    bd::restore_put(_backend, d, std::move(*parsed));
                });
            }
        }
        // A backup deleted under a restore leaves the table as loaded.
        bd::finish_restore(_backend, d);
    }

    Backend& _backend;
    std::shared_ptr<backup_store> _store;
    backup_worker_options _options;
    std::atomic<bool> _poked{true};
    std::chrono::steady_clock::time_point _next_list{};
};

}  // namespace kythira::dynamodb
