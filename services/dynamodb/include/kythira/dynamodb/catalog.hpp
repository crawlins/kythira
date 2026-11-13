// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file catalog.hpp
/// @brief DynamoDB's table catalog: descriptors, status transitions, table
///        ids and background jobs (`.kiro/specs/dynamodb-compatible-api/`
///        task 6, Requirements 7.2-7.4).
///
/// The catalog is the reserved range `\0meta/dynamodb/` of the node's
/// `kv_engine`, small enough for one group, and changes only through Raft
/// entries (`catalog_command`), so it is linearizable (7.2). Each table has
/// a versioned descriptor under `table/<name>`.
///
/// - Table ids come from a counter under `next-table-id` that only grows,
///   written in the same batch as the descriptor, so an id is never reused
///   and a recreated table never sees an old table's data (7.4).
/// - Status transitions are entries. `CreateTable` writes `CREATING`, and
///   `UpdateTable` writes `UPDATING` when the change needs work. The catalog
///   leader then proposes `set_status` once the work is done. SDK waiters
///   poll that status.
/// - `DeleteTable` marks the descriptor `DELETING` and records a delete job
///   under `job/<id>`. The leader drives the job with bounded
///   `drop_table_step` entries on the item groups and records each step as
///   a `job_progress` entry, so a new leader resumes where the old one
///   stopped (7.3). The last step erases the job and the descriptor, and
///   only then may the name be reused.
///
/// A follow-on entry names the table id it acts on and does nothing when
/// the stored table has another id, so a stale entry from a former leader
/// cannot touch a recreated table.
///
/// Global secondary indexes (task 11) take their ids from the same counter
/// as tables, so an index key space is never reused either. Deleting a
/// table records one delete job per index beside the table's own; an
/// index's job drops its key space and never touches the descriptor.

#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/global_index.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/item_state_machine.hpp>
#include <kythira/dynamodb/key_layout.hpp>
#include <kythira/dynamodb/vector_index.hpp>

#include <raft/memory_kv_engine.hpp>
#include <raft/persistent_state_machine.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

enum class table_status : std::uint8_t {
    creating = 0,
    active = 1,
    updating = 2,
    deleting = 3,
};

[[nodiscard]] inline auto status_name(table_status s) -> std::string_view {
    switch (s) {
        case table_status::creating:
            return "CREATING";
        case table_status::active:
            return "ACTIVE";
        case table_status::updating:
            return "UPDATING";
        case table_status::deleting:
            return "DELETING";
    }
    return "ACTIVE";
}

enum class billing_mode : std::uint8_t {
    provisioned = 0,
    pay_per_request = 1,
};

enum class table_class : std::uint8_t {
    standard = 0,
    standard_infrequent_access = 1,
};

/// A table's time to live (task 13): which attribute holds each item's
/// expiry, in seconds since the epoch.
struct time_to_live {
    bool enabled = false;
    /// The attribute while enabled; empty while disabled.
    std::string attribute;
    /// When `UpdateTimeToLive` last changed it, from the entry; 0 never.
    std::uint64_t changed_ms = 0;

    friend auto operator==(const time_to_live&, const time_to_live&) -> bool = default;
};

/// What a DynamoDB stream's records carry (`StreamViewType`).
enum class stream_view_type : std::uint8_t {
    keys_only = 1,
    new_image = 2,
    old_image = 3,
    new_and_old_images = 4,
};

/// One of a table's DynamoDB streams (task 16): enabled, or disabled and
/// still listed while its records may be read.
struct table_stream {
    /// When it was enabled (ms since the epoch, from the entry): its
    /// `StreamLabel`, unique per table.
    std::uint64_t label_ms = 0;
    stream_view_type view = stream_view_type::keys_only;
    /// When it was disabled; 0 while enabled. In a command, nonzero asks
    /// for the table's stream to be disabled.
    std::uint64_t disabled_ms = 0;

    friend auto operator==(const table_stream&, const table_stream&) -> bool = default;
};

/// How long a disabled stream stays listed: as long as its last records
/// can be read.
inline constexpr std::uint64_t k_stream_retention_ms = std::uint64_t{24} * 60 * 60 * 1000;

/// A Kinesis streaming destination's state (`DestinationStatus`).
enum class destination_status : std::uint8_t {
    enabling = 1,
    active = 2,
    disabling = 3,
    disabled = 4,
    enable_failed = 5,
    updating = 6,
};

/// The precision of `ApproximateCreationDateTime` in the records a
/// destination receives.
enum class time_precision : std::uint8_t {
    millisecond = 1,
    microsecond = 2,
};

/// One of a table's Kinesis streaming destinations (task 17). AWS lets a
/// table stream to one Kinesis data stream at a time; disabled and failed
/// ones stay listed, one entry per stream ARN.
struct kinesis_destination {
    std::string stream_arn;
    destination_status status = destination_status::enabling;
    time_precision precision = time_precision::millisecond;
    /// Why it failed to enable; empty otherwise.
    std::string status_description;
    /// When it was enabled (ms since the epoch, from the entry): records
    /// written from then on are published.
    std::uint64_t enabled_ms = 0;
    /// How far the publisher got in each change log stream: the next
    /// sequence number to publish, by store stream key. Cleared on disable.
    std::vector<std::pair<std::string, std::uint64_t>> cursors;

    /// Whether the table's writes must be logged for it.
    [[nodiscard]] auto streaming() const -> bool {
        return status == destination_status::enabling || status == destination_status::active ||
               status == destination_status::updating;
    }

    friend auto operator==(const kinesis_destination&, const kinesis_destination&)
        -> bool = default;
};

/// Where a restored table came from (tasks 18 and 19): `RestoreSummary`.
struct table_restore {
    /// The backup restored from; empty for a point-in-time restore; the
    /// import's ARN for an imported table.
    std::string source_backup_arn;
    /// Empty for a table that was not restored.
    std::string source_table_arn;
    /// When the restore was asked for (ms since the epoch, from the entry).
    std::uint64_t restore_ms = 0;
    /// The items are still being loaded: the table stays CREATING.
    bool in_progress = false;
    /// A point-in-time restore (task 19): the source table's id and the
    /// time restored to (ms since the epoch); both 0 for a backup's.
    std::uint64_t source_table_id = 0;
    std::uint64_t point_ms = 0;

    [[nodiscard]] auto restored() const -> bool { return !source_table_arn.empty(); }
    /// An `ImportTable` table (task 20): `source_backup_arn` holds the
    /// import's ARN (`.../import/<id>`), and there is no restore to report.
    [[nodiscard]] auto imported() const -> bool {
        return source_backup_arn.find("/import/") != std::string::npos;
    }

    friend auto operator==(const table_restore&, const table_restore&) -> bool = default;
};

/// Point-in-time recovery (task 19): `UpdateContinuousBackups`, and the
/// archiver's progress. Each enable starts a new archive under
/// `pitr/<table id>/<enabled_ms>/` in the backup store.
struct continuous_backups {
    bool enabled = false;
    /// `RecoveryPeriodInDays`, 1 to 35.
    std::uint32_t recovery_days = 35;
    /// When it was enabled (ms since the epoch, from the entry); names the
    /// archive.
    std::uint64_t enabled_ms = 0;
    /// The end of the oldest base the archive keeps (0 until the first is
    /// taken): no restore can go earlier.
    std::uint64_t earliest_ms = 0;
    /// The archive holds every change record written up to here.
    std::uint64_t latest_ms = 0;
    /// The number the next archive object takes.
    std::uint64_t next_object = 0;
    /// The archiver's position in each store stream of the change log.
    std::vector<std::pair<std::string, std::uint64_t>> cursors;

    friend auto operator==(const continuous_backups&, const continuous_backups&) -> bool = default;
};

/// A replica of a global table (task 22) in another Kythira cluster,
/// version 2019.11.21. `replica_status` holds AWS's `ReplicaStatus`
/// values that apply here.
enum class replica_status : std::uint8_t {
    creating = 0,
    active = 1,
    deleting = 2,
    creation_failed = 3,
};

struct table_replica {
    /// The peer's region name, as `UpdateTable` names it.
    std::string region;
    replica_status status = replica_status::creating;
    /// When it was asked for (ms since the epoch, from the entry).
    std::uint64_t created_ms = 0;
    /// The replicator has copied the table's items there (set once the
    /// replica is created; the change log carries the rest).
    bool backfilled = false;
    /// The replicator's position in each store stream of the change log,
    /// for this replica.
    std::vector<std::pair<std::string, std::uint64_t>> cursors;
    /// Why creating it failed (`creation_failed`).
    std::string failure;

    friend auto operator==(const table_replica&, const table_replica&) -> bool = default;
};

/// A multi-region strongly consistent table's witness (task 27): a region
/// whose strong voters store the table but serve no reads or writes.
enum class witness_status : std::uint8_t {
    creating = 0,
    active = 1,
    deleting = 2,
};

struct table_witness {
    std::string region;
    witness_status status = witness_status::creating;

    friend auto operator==(const table_witness&, const table_witness&) -> bool = default;
};

[[nodiscard]] inline auto witness_status_name(witness_status s) -> std::string_view {
    switch (s) {
        case witness_status::creating:
            return "CREATING";
        case witness_status::active:
            return "ACTIVE";
        case witness_status::deleting:
            return "DELETING";
    }
    return "CREATING";
}

[[nodiscard]] inline auto replica_status_name(replica_status s) -> std::string_view {
    switch (s) {
        case replica_status::creating:
            return "CREATING";
        case replica_status::active:
            return "ACTIVE";
        case replica_status::deleting:
            return "DELETING";
        case replica_status::creation_failed:
            return "CREATION_FAILED";
    }
    return "ACTIVE";
}

/// A resource-based policy (task 21) on the table or on one of its
/// streams. The authorizer (IAM's evaluator) reads it on every request to
/// the resource; the catalog only keeps it.
struct resource_policy {
    /// 0 for the table's own policy, else the `label_ms` of the stream it
    /// is attached to.
    std::uint64_t stream_label = 0;
    /// The document, byte for byte as given.
    std::string document;
    /// `RevisionId`: decimal milliseconds since the epoch, growing with
    /// each change of the document. In a `set_resource_policy` command it
    /// is the `ExpectedRevisionId` instead (empty when none was given).
    std::string revision;

    friend auto operator==(const resource_policy&, const resource_policy&) -> bool = default;
};

/// `ContributorInsightsMode`.
enum class insights_mode : std::uint8_t {
    accessed_and_throttled = 0,
    throttled = 1,
};

/// Contributor Insights (task 21) on the table or one of its GSIs.
struct contributor_insights {
    /// Empty for the table.
    std::string index;
    bool enabled = false;
    insights_mode mode = insights_mode::accessed_and_throttled;
    /// When it was last enabled or disabled (ms since the epoch, from the
    /// entry): `LastUpdateDateTime`, and the suffix of the rule names.
    std::uint64_t updated_ms = 0;

    friend auto operator==(const contributor_insights&, const contributor_insights&)
        -> bool = default;
};

/// What the catalog knows about a table. Later tasks add replicas; each
/// new field gets a new descriptor format version.
struct table_descriptor {
    std::string name;
    std::uint64_t table_id = 0;
    key_schema schema;
    table_status status = table_status::creating;
    /// Milliseconds since the epoch, from the entry (never the replica's
    /// clock).
    std::uint64_t created_ms = 0;
    billing_mode billing = billing_mode::provisioned;
    std::uint64_t read_capacity = 0;
    std::uint64_t write_capacity = 0;
    table_class storage_class = table_class::standard;
    bool deletion_protection = false;
    /// The warm throughput the table was asked for, in units per second;
    /// 0 when never set. `DescribeTable` shows at least AWS's defaults.
    std::uint64_t warm_read = 0;
    std::uint64_t warm_write = 0;
    /// Local secondary indexes, fixed at creation (task 9).
    std::vector<local_index> lsis;
    /// Global secondary indexes (task 11).
    std::vector<global_index> gsis;
    std::vector<std::pair<std::string, std::string>> tags;
    /// Bumped by every entry that changes the descriptor.
    std::uint64_t revision = 0;
    /// Time to live (task 13).
    time_to_live ttl;
    /// DynamoDB streams, oldest first (task 16): the last is the table's
    /// latest, enabled unless it has `disabled_ms`.
    std::vector<table_stream> streams;
    /// Kinesis streaming destinations (task 17), at most one streaming.
    std::vector<kinesis_destination> destinations;
    /// Set on a table restored from a backup or a point in time (tasks 18
    /// and 19).
    table_restore restore;
    /// Point-in-time recovery (task 19).
    continuous_backups pitr;
    /// Resource-based policies (task 21): at most one for the table and
    /// one for each stream.
    std::vector<resource_policy> policies;
    /// Contributor Insights settings (task 21): one for the table and for
    /// each GSI it was ever set on.
    std::vector<contributor_insights> insights;
    /// Vector indexes (task 26), in creation order.
    std::vector<vector_index> vectors;
    /// A global table (task 22): this cluster's region, and the replicas
    /// in other clusters. Empty for any other table.
    std::string region;
    std::vector<table_replica> replicas;
    /// Multi-region strong consistency (task 27): the table's items live in
    /// the strong host under strong ids, and `replicas` are the other
    /// regions'. In the strong catalog `region` is empty and `replicas`
    /// lists every replica region.
    bool strong = false;
    /// An MRSC table's witness region, at most one.
    std::vector<table_witness> witnesses;
    /// The ids the table and its indexes had in the cluster's host before
    /// it was made strong, each with the strong id that replaced it. Kept
    /// until `salvage_after_ms`, when the worker moves any item a request
    /// in flight wrote under an old id and drops the old key spaces.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> retired_ids;
    std::uint64_t salvage_after_ms = 0;

    /// The witness in `name`, or null.
    [[nodiscard]] auto witness_in(std::string_view name) const -> const table_witness* {
        const auto it = std::find_if(witnesses.begin(), witnesses.end(),
                                     [&](const table_witness& w) { return w.region == name; });
        return it == witnesses.end() ? nullptr : &*it;
    }

    /// The replica in `name`, or null.
    [[nodiscard]] auto replica_in(std::string_view name) const -> const table_replica* {
        const auto it = std::find_if(replicas.begin(), replicas.end(),
                                     [&](const table_replica& r) { return r.region == name; });
        return it == replicas.end() ? nullptr : &*it;
    }

    /// Whether this is a global table.
    [[nodiscard]] auto global() const -> bool { return !region.empty(); }

    /// The vector index named `name`, or null.
    [[nodiscard]] auto vector_named(std::string_view index) const -> const vector_index* {
        const auto it = std::find_if(vectors.begin(), vectors.end(),
                                     [&](const vector_index& v) { return v.name == index; });
        return it == vectors.end() ? nullptr : &*it;
    }

    /// The policy on the table (`stream_label` 0) or on a stream, or null.
    [[nodiscard]] auto policy_of(std::uint64_t stream_label) const -> const resource_policy* {
        const auto it =
            std::find_if(policies.begin(), policies.end(),
                         [&](const resource_policy& p) { return p.stream_label == stream_label; });
        return it == policies.end() ? nullptr : &*it;
    }

    /// The Contributor Insights setting of the table (`index` empty) or a
    /// GSI, or null when it was never set.
    [[nodiscard]] auto insights_of(std::string_view index) const -> const contributor_insights* {
        const auto it =
            std::find_if(insights.begin(), insights.end(),
                         [&](const contributor_insights& c) { return c.index == index; });
        return it == insights.end() ? nullptr : &*it;
    }

    /// The enabled stream, or null.
    [[nodiscard]] auto enabled_stream() const -> const table_stream* {
        return !streams.empty() && streams.back().disabled_ms == 0 ? &streams.back() : nullptr;
    }

    /// Whether writes to the table append change records (task 15,
    /// `change_log.hpp`): while a DynamoDB stream is enabled or a Kinesis
    /// destination is streaming (task 17), or point-in-time recovery is
    /// enabled (task 19), or the table has a vector index, whose graphs
    /// the log feeds (task 26), or the table is global with last-writer-wins
    /// replication from the log (task 22). A strongly consistent table
    /// (task 27) replicates through Raft, not the log.
    [[nodiscard]] auto logs_changes() const -> bool {
        return enabled_stream() != nullptr || streaming_destination() != nullptr || pitr.enabled ||
               !vectors.empty() || (global() && !strong);
    }

    /// The Kinesis destination being streamed to, or null.
    [[nodiscard]] auto streaming_destination() const -> const kinesis_destination* {
        const auto it = std::find_if(destinations.begin(), destinations.end(),
                                     [](const kinesis_destination& k) { return k.streaming(); });
        return it == destinations.end() ? nullptr : &*it;
    }

    /// The label a logged write carries: the enabled stream's, else 0.
    [[nodiscard]] auto stream_label() const -> std::uint64_t {
        const auto* s = enabled_stream();
        return s == nullptr ? 0 : s->label_ms;
    }

    friend auto operator==(const table_descriptor&, const table_descriptor&) -> bool = default;
};

/// A background delete in progress.
struct delete_job {
    std::uint64_t table_id = 0;
    std::string name;
    key_schema schema;
    std::uint64_t deleted = 0;

    friend auto operator==(const delete_job&, const delete_job&) -> bool = default;
};

/// One change to the catalog.
struct catalog_command {
    enum class kind : std::uint8_t {
        create_table = 1,
        set_status = 2,
        update_table = 3,
        delete_table = 4,
        job_progress = 5,
        /// `TagResource`: `table.tags` are added or overwrite same-keyed
        /// tags.
        tag_resource = 6,
        /// `UntagResource`: the keys of `table.tags` are removed.
        untag_resource = 7,
        /// `UpdateTimeToLive` (task 13): `table.ttl` holds the request's
        /// `enabled` and `attribute`. Refused as AWS refuses it: enabling
        /// what is enabled, disabling what is disabled, naming another
        /// attribute while enabled, or a change less than
        /// `ttl_min_interval_ms` after the last one.
        set_ttl = 8,
        /// `EnableKinesisStreamingDestination` and its siblings (task 17):
        /// `table.destinations` holds one entry whose `status` names the
        /// request: `enabling` to enable its stream ARN with its precision,
        /// `disabling` to disable it, `updating` to change its precision.
        set_destination = 9,
        /// The publisher's progress (task 17): `table.destinations` holds
        /// one entry, by stream ARN, with the status to move to (`active`
        /// or `enable_failed` from `enabling`, `active` from `updating`,
        /// `disabled` from `disabling`, or the current one) and its
        /// cursors, which replace the stored ones, never moving one back.
        destination_progress = 10,
        /// A restore (task 18) finished loading `table` (by name and
        /// `table_id`): the table may become ACTIVE.
        restore_done = 11,
        /// `UpdateContinuousBackups` (task 19): `table.pitr` holds the
        /// request's `enabled` and `recovery_days`. An enable while enabled
        /// changes only the period; a fresh enable starts a new archive.
        set_pitr = 12,
        /// The archiver's progress (task 19): `table.pitr` with the
        /// `enabled_ms` of the archive it wrote, its cursors (never moved
        /// back), `next_object`, `latest_ms` and `earliest_ms` (each only
        /// moving forward). Progress for an archive since replaced is
        /// ignored.
        pitr_progress = 13,
        /// `PutResourcePolicy` and `DeleteResourcePolicy` (task 21):
        /// `table.policies` holds one entry naming the resource by
        /// `stream_label`, its new document (empty to delete) and, in
        /// `revision`, the `ExpectedRevisionId` (empty for none; `NO_POLICY`
        /// to require that none exists). A mismatch is refused with
        /// `PolicyNotFoundException`. Putting the document already there
        /// keeps its revision; deleting a policy that is not there changes
        /// nothing.
        set_resource_policy = 14,
        /// `UpdateContributorInsights` (task 21): `table.insights` holds one
        /// entry with the index (empty for the table), `enabled` and
        /// `mode`; the entry's time becomes `updated_ms`.
        set_contributor_insights = 15,
        /// `ReplicaUpdates` (task 22): `table.replicas` is the list the
        /// table should have and `table.region` this cluster's region. A
        /// region the table lacks is added CREATING, an ACTIVE or failed
        /// replica the command lacks starts DELETING, a DELETING one the
        /// command lacks goes, and a replica's command copy may move it
        /// from CREATING to ACTIVE or CREATION_FAILED (with `failure`) or
        /// mark it `backfilled`. An added replica keeps the command's
        /// cursors, and its ACTIVE status when the command gives it (a
        /// peer's notice of a replica another cluster created). One replica may be added or removed
        /// at a time, and none while another is being. With no replicas left the table is no longer
        /// global.
        set_replicas = 16,
        /// The replicator's progress (task 22): each replica of
        /// `table.replicas` names its region and cursors, which move
        /// forward; nothing else changes.
        replica_progress = 17,
        /// The strong catalog's first entry (task 27): the next table id
        /// becomes at least `table.table_id`, so the strong host's ids start
        /// at `k_strong_table_id_base`. Proposed by every strong voter at
        /// start; only the first changes anything.
        seed_ids = 18,
        /// `UpdateTable` made the table multi-region strongly consistent
        /// (task 27), in the region it was asked in: `table` names it and
        /// carries its strong `table_id`, its GSIs with their strong ids (by
        /// name), its `region`, the other regions' `replicas` (CREATING),
        /// its `witnesses`, `retired_ids` (the current ids each with its
        /// strong one) and `salvage_after_ms`. Refused unless the table is
        /// ACTIVE, neither global nor strong, and still has those ids.
        make_strong = 19,
        /// A replica region's catalog takes a strong table the strong
        /// catalog lists for it (task 27): `table` is the whole descriptor,
        /// stored ACTIVE as given. Refused when the name is taken.
        adopt_strong = 20,
        /// The members of a strong table (task 27): the statuses (and
        /// failures) of `table.replicas` and `table.witnesses` replace the
        /// stored ones, by region.
        strong_members = 21,
        /// The worker salvaged the items written under a strong table's
        /// retired ids (task 27): `retired_ids` go, each old key space gets
        /// a delete job.
        salvage_done = 22,
    };

    /// Which settings an `update_table` changes.
    enum update_field : std::uint8_t {
        update_billing = 1U << 0U,
        update_throughput = 1U << 1U,
        update_class = 1U << 2U,
        update_deletion_protection = 1U << 3U,
        update_warm_throughput = 1U << 4U,
        /// `GlobalSecondaryIndexUpdates`: `table.gsis` is the list the
        /// table should have. A name the table lacks is created (CREATING,
        /// with a new key space), a name the command lacks is deleted
        /// (DELETING, with a delete job for its key space), an index whose
        /// command copy is ACTIVE while it is CREATING becomes ACTIVE (the
        /// backfill finished), and a provisioned index takes the command's
        /// throughput. One index may be created or deleted at a time.
        update_gsis = 1U << 5U,
        /// `StreamSpecification` (task 16): `table.streams` holds one entry,
        /// enabling a stream of its view type, or with `disabled_ms` set,
        /// disabling the table's.
        update_stream = 1U << 6U,
        /// `VectorIndexUpdates` (task 26): `table.vectors` is the list the
        /// table should have, as for `update_gsis`. A name the table lacks
        /// is created (CREATING), an ACTIVE index the command lacks starts
        /// DELETING, a DELETING index the command lacks is dropped, and a
        /// CREATING index whose command copy is ACTIVE becomes ACTIVE (its
        /// graphs can be built). One index may be created or deleted at a
        /// time. Graphs live in memory on each node and are built from the
        /// table and its change log, so no key space or job is needed.
        update_vectors = 1U << 7U,
    };

    kind op = kind::create_table;
    /// `create_table`: the new table (its id, status, revision and creation
    /// time are the catalog's to set); `update_table`: the name and the new
    /// settings; `set_status` and `job_progress`: name and `table_id`.
    table_descriptor table;
    std::uint8_t update_fields = 0;
    /// `set_status`: the status to leave and the one to enter.
    table_status from = table_status::creating;
    table_status to = table_status::active;
    std::uint64_t now_ms = 0;
    /// `job_progress`: items deleted by the step, and whether the table is
    /// now empty on every group.
    std::uint64_t deleted = 0;
    bool done = false;
    /// `set_ttl`: the shortest time allowed between two changes (AWS: an
    /// hour); 0 allows any.
    std::uint64_t ttl_min_interval_ms = 0;

    friend auto operator==(const catalog_command&, const catalog_command&) -> bool = default;
};

/// What `apply()` returns for a `catalog_command`.
struct catalog_result {
    /// Empty on success, else the error type (a full Coral name) and
    /// message for the client.
    std::string error_type;
    std::string error_message;
    /// The descriptor after the entry, when the table still exists.
    std::optional<table_descriptor> table;

    [[nodiscard]] auto ok() const -> bool { return error_type.empty(); }

    friend auto operator==(const catalog_result&, const catalog_result&) -> bool = default;
};

namespace catalog_detail {

/// 2 added `gsis` (task 11) to the descriptor and to the commands and
/// results that carry one; 3 added `ttl` after the revision (task 13), and
/// the `set_ttl` command with its interval; 4 added `streams` after `ttl`,
/// and `update_stream` (task 16). Older versions still decode. A form whose
/// table has no stream is still written as 3, so a replica not yet
/// upgraded keeps applying everything but streams. 5 added `destinations`
/// after `streams`, with the `set_destination` and `destination_progress`
/// commands (task 17); a form without destinations keeps its older
/// version the same way. 6 added `restore` after `destinations`, with the
/// `restore_done` command (task 18), likewise only for a restored table.
/// 7 added `pitr` after `restore`, with `set_pitr` and `pitr_progress`
/// (task 19), only for a table that ever had point-in-time recovery.
/// 8 added `policies` and `insights` after `pitr`, with
/// `set_resource_policy` and `set_contributor_insights` (task 21), only
/// for a table that has either.
/// 9 added `vectors` after `insights`, with
/// `update_vectors` (task 26), only for a table that has a vector index.
/// 10 added `region` and `replicas` after `vectors`, with `set_replicas`
/// and `replica_progress` (task 22), only for a global table.
/// 11 added `strong`, `witnesses`, `retired_ids` and `salvage_after_ms`
/// after `replicas`, with `seed_ids`, `make_strong`, `adopt_strong`,
/// `strong_members` and `salvage_done` (task 27), only for a
/// multi-region strongly consistent table.
inline constexpr std::uint8_t k_descriptor_format_version = 11;
inline constexpr std::uint8_t k_job_format_version = 1;
inline constexpr std::uint8_t k_command_format_version = 11;
inline constexpr std::uint8_t k_result_format_version = 11;

/// The version a form carrying `d` is written as.
[[nodiscard]] inline auto version_for(const table_descriptor& d) -> std::uint8_t {
    if (d.strong || !d.witnesses.empty() || !d.retired_ids.empty()) {
        return 11;
    }
    if (!d.region.empty() || !d.replicas.empty()) {
        return 10;
    }
    if (!d.vectors.empty()) {
        return 9;
    }
    if (!d.policies.empty() || !d.insights.empty()) {
        return 8;
    }
    if (d.pitr != continuous_backups{}) {
        return 7;
    }
    if (d.restore.restored()) {
        return 6;
    }
    if (!d.destinations.empty()) {
        return 5;
    }
    return d.streams.empty() ? 3 : 4;
}

[[nodiscard]] inline auto table_key(std::string_view name) -> std::string {
    return std::string(k_catalog_prefix) + "table/" + std::string(name);
}
[[nodiscard]] inline auto tables_range() -> kythira::key_range {
    return prefix_range(std::string(k_catalog_prefix) + "table/");
}
[[nodiscard]] inline auto job_key(std::uint64_t table_id) -> std::string {
    // Big-endian, so jobs run oldest first.
    std::string out = std::string(k_catalog_prefix) + "job/";
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((table_id >> shift) & 0xFFU));
    }
    return out;
}
[[nodiscard]] inline auto jobs_range() -> kythira::key_range {
    return prefix_range(std::string(k_catalog_prefix) + "job/");
}
[[nodiscard]] inline auto next_id_key() -> std::string {
    return std::string(k_catalog_prefix) + "next-table-id";
}

[[nodiscard]] inline auto u64_string(std::uint64_t v) -> std::string {
    std::string out;
    item_sm_detail::put_u64(out, v);
    return out;
}

[[nodiscard]] inline auto get_flag(detail::reader& r) -> std::optional<bool> {
    const auto b = r.byte();
    if (!b || *b > 1) {
        return std::nullopt;
    }
    return *b == 1;
}

inline void put_descriptor_body(std::string& out, const table_descriptor& d, std::uint8_t version) {
    detail::put_bytes(out, d.name);
    detail::put_varint(out, d.table_id);
    item_sm_detail::put_schema(out, d.schema);
    out.push_back(static_cast<char>(d.status));
    detail::put_varint(out, d.created_ms);
    out.push_back(static_cast<char>(d.billing));
    detail::put_varint(out, d.read_capacity);
    detail::put_varint(out, d.write_capacity);
    out.push_back(static_cast<char>(d.storage_class));
    out.push_back(d.deletion_protection ? '\1' : '\0');
    detail::put_varint(out, d.warm_read);
    detail::put_varint(out, d.warm_write);
    put_local_indexes(out, d.lsis);
    put_global_indexes(out, d.gsis);
    detail::put_varint(out, d.tags.size());
    for (const auto& [k, v] : d.tags) {
        detail::put_bytes(out, k);
        detail::put_bytes(out, v);
    }
    detail::put_varint(out, d.revision);
    out.push_back(d.ttl.enabled ? '\1' : '\0');
    detail::put_bytes(out, d.ttl.attribute);
    detail::put_varint(out, d.ttl.changed_ms);
    if (version >= 4) {
        detail::put_varint(out, d.streams.size());
        for (const auto& s : d.streams) {
            detail::put_varint(out, s.label_ms);
            out.push_back(static_cast<char>(s.view));
            detail::put_varint(out, s.disabled_ms);
        }
    }
    if (version >= 5) {
        detail::put_varint(out, d.destinations.size());
        for (const auto& k : d.destinations) {
            detail::put_bytes(out, k.stream_arn);
            out.push_back(static_cast<char>(k.status));
            out.push_back(static_cast<char>(k.precision));
            detail::put_bytes(out, k.status_description);
            detail::put_varint(out, k.enabled_ms);
            detail::put_varint(out, k.cursors.size());
            for (const auto& [key, seq] : k.cursors) {
                detail::put_bytes(out, key);
                detail::put_varint(out, seq);
            }
        }
    }
    if (version >= 6) {
        detail::put_bytes(out, d.restore.source_backup_arn);
        detail::put_bytes(out, d.restore.source_table_arn);
        detail::put_varint(out, d.restore.restore_ms);
        out.push_back(d.restore.in_progress ? '\1' : '\0');
        detail::put_varint(out, d.restore.source_table_id);
        detail::put_varint(out, d.restore.point_ms);
    }
    if (version >= 7) {
        out.push_back(d.pitr.enabled ? '\1' : '\0');
        detail::put_varint(out, d.pitr.recovery_days);
        detail::put_varint(out, d.pitr.enabled_ms);
        detail::put_varint(out, d.pitr.earliest_ms);
        detail::put_varint(out, d.pitr.latest_ms);
        detail::put_varint(out, d.pitr.next_object);
        detail::put_varint(out, d.pitr.cursors.size());
        for (const auto& [key, seq] : d.pitr.cursors) {
            detail::put_bytes(out, key);
            detail::put_varint(out, seq);
        }
    }
    if (version >= 8) {
        detail::put_varint(out, d.policies.size());
        for (const auto& p : d.policies) {
            detail::put_varint(out, p.stream_label);
            detail::put_bytes(out, p.document);
            detail::put_bytes(out, p.revision);
        }
        detail::put_varint(out, d.insights.size());
        for (const auto& c : d.insights) {
            detail::put_bytes(out, c.index);
            out.push_back(c.enabled ? '\1' : '\0');
            out.push_back(static_cast<char>(c.mode));
            detail::put_varint(out, c.updated_ms);
        }
    }
    if (version >= 9) {
        put_vector_indexes(out, d.vectors);
    }
    if (version >= 10) {
        detail::put_bytes(out, d.region);
        detail::put_varint(out, d.replicas.size());
        for (const auto& r : d.replicas) {
            detail::put_bytes(out, r.region);
            out.push_back(static_cast<char>(r.status));
            detail::put_varint(out, r.created_ms);
            out.push_back(r.backfilled ? '\1' : '\0');
            detail::put_varint(out, r.cursors.size());
            for (const auto& [key, seq] : r.cursors) {
                detail::put_bytes(out, key);
                detail::put_varint(out, seq);
            }
            detail::put_bytes(out, r.failure);
        }
    }
    if (version >= 11) {
        out.push_back(d.strong ? '\1' : '\0');
        detail::put_varint(out, d.witnesses.size());
        for (const auto& w : d.witnesses) {
            detail::put_bytes(out, w.region);
            out.push_back(static_cast<char>(w.status));
        }
        detail::put_varint(out, d.retired_ids.size());
        for (const auto& [from, to] : d.retired_ids) {
            detail::put_varint(out, from);
            detail::put_varint(out, to);
        }
        detail::put_varint(out, d.salvage_after_ms);
    }
}

/// `version` is the format of the enclosing form (descriptor, command or
/// result), all of which gained `gsis` at 2 and `ttl` at 3.
[[nodiscard]] inline auto get_descriptor_body(detail::reader& r, std::uint8_t version)
    -> std::optional<table_descriptor> {
    table_descriptor d;
    auto name = r.bytes();
    const auto id = r.varint();
    auto schema = item_sm_detail::get_schema(r);
    const auto status = r.byte();
    const auto created = r.varint();
    const auto billing = r.byte();
    const auto rcu = r.varint();
    const auto wcu = r.varint();
    const auto cls = r.byte();
    const auto protection = get_flag(r);
    const auto warm_read = r.varint();
    const auto warm_write = r.varint();
    auto lsis = get_local_indexes(r);
    std::optional<std::vector<global_index>> gsis;
    if (lsis) {
        gsis = version >= 2 ? get_global_indexes(r) : std::vector<global_index>{};
    }
    const auto tag_count = gsis ? r.count() : std::nullopt;
    if (!name || !id || !schema || !status || *status > 3 || !created || !billing || *billing > 1 ||
        !rcu || !wcu || !cls || *cls > 1 || !protection || !warm_read || !warm_write || !lsis ||
        !tag_count) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < *tag_count; ++i) {
        auto k = r.bytes();
        auto v = r.bytes();
        if (!k || !v) {
            return std::nullopt;
        }
        d.tags.emplace_back(std::move(*k), std::move(*v));
    }
    const auto revision = r.varint();
    if (!revision) {
        return std::nullopt;
    }
    if (version >= 3) {
        const auto enabled = get_flag(r);
        auto attribute = r.bytes();
        const auto changed = r.varint();
        if (!enabled || !attribute || !changed) {
            return std::nullopt;
        }
        d.ttl.enabled = *enabled;
        d.ttl.attribute = std::move(*attribute);
        d.ttl.changed_ms = *changed;
    }
    if (version >= 4) {
        const auto n = r.count();
        if (!n) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < *n; ++i) {
            const auto label = r.varint();
            const auto view = r.byte();
            const auto disabled = r.varint();
            if (!label || !view || *view < 1 || *view > 4 || !disabled) {
                return std::nullopt;
            }
            d.streams.push_back(
                table_stream{*label, static_cast<stream_view_type>(*view), *disabled});
        }
    }
    if (version >= 5) {
        const auto n = r.count();
        if (!n) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < *n; ++i) {
            kinesis_destination k;
            auto arn = r.bytes();
            const auto status = r.byte();
            const auto precision = r.byte();
            auto description = r.bytes();
            const auto enabled = r.varint();
            const auto cursors = enabled ? r.count() : std::nullopt;
            if (!arn || !status || *status < 1 || *status > 6 || !precision || *precision < 1 ||
                *precision > 2 || !description || !cursors) {
                return std::nullopt;
            }
            for (std::size_t j = 0; j < *cursors; ++j) {
                auto key = r.bytes();
                const auto seq = r.varint();
                if (!key || !seq) {
                    return std::nullopt;
                }
                k.cursors.emplace_back(std::move(*key), *seq);
            }
            k.stream_arn = std::move(*arn);
            k.status = static_cast<destination_status>(*status);
            k.precision = static_cast<time_precision>(*precision);
            k.status_description = std::move(*description);
            k.enabled_ms = *enabled;
            d.destinations.push_back(std::move(k));
        }
    }
    if (version >= 6) {
        auto backup = r.bytes();
        auto table = r.bytes();
        const auto at = r.varint();
        const auto loading = get_flag(r);
        const auto source_id = r.varint();
        const auto point = r.varint();
        if (!backup || !table || !at || !loading || !source_id || !point) {
            return std::nullopt;
        }
        d.restore =
            table_restore{std::move(*backup), std::move(*table), *at, *loading, *source_id, *point};
    }
    if (version >= 7) {
        const auto enabled = get_flag(r);
        const auto days = r.varint();
        const auto since = r.varint();
        const auto earliest = r.varint();
        const auto latest = r.varint();
        const auto next = r.varint();
        const auto cursors = r.count();
        if (!enabled || !days || *days > 35 || !since || !earliest || !latest || !next ||
            !cursors) {
            return std::nullopt;
        }
        for (std::size_t j = 0; j < *cursors; ++j) {
            auto key = r.bytes();
            const auto seq = r.varint();
            if (!key || !seq) {
                return std::nullopt;
            }
            d.pitr.cursors.emplace_back(std::move(*key), *seq);
        }
        d.pitr.enabled = *enabled;
        d.pitr.recovery_days = static_cast<std::uint32_t>(*days);
        d.pitr.enabled_ms = *since;
        d.pitr.earliest_ms = *earliest;
        d.pitr.latest_ms = *latest;
        d.pitr.next_object = *next;
    }
    if (version >= 8) {
        const auto n = r.count();
        if (!n) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < *n; ++i) {
            const auto label = r.varint();
            auto document = r.bytes();
            auto revision = r.bytes();
            if (!label || !document || !revision) {
                return std::nullopt;
            }
            d.policies.push_back(
                resource_policy{*label, std::move(*document), std::move(*revision)});
        }
        const auto m = r.count();
        if (!m) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < *m; ++i) {
            auto index = r.bytes();
            const auto enabled = get_flag(r);
            const auto mode = r.byte();
            const auto updated = r.varint();
            if (!index || !enabled || !mode || *mode > 1 || !updated) {
                return std::nullopt;
            }
            d.insights.push_back(contributor_insights{std::move(*index), *enabled,
                                                      static_cast<insights_mode>(*mode), *updated});
        }
    }
    if (version >= 9) {
        auto vectors = get_vector_indexes(r);
        if (!vectors) {
            return std::nullopt;
        }
        d.vectors = std::move(*vectors);
    }
    if (version >= 10) {
        auto region = r.bytes();
        const auto n = r.count();
        if (!region || !n) {
            return std::nullopt;
        }
        d.region = std::move(*region);
        for (std::size_t i = 0; i < *n; ++i) {
            table_replica rep;
            auto name = r.bytes();
            const auto status = r.byte();
            const auto created = r.varint();
            const auto backfilled = get_flag(r);
            const auto m = r.count();
            if (!name || !status || *status > 3 || !created || !backfilled || !m) {
                return std::nullopt;
            }
            for (std::size_t j = 0; j < *m; ++j) {
                auto key = r.bytes();
                const auto seq = r.varint();
                if (!key || !seq) {
                    return std::nullopt;
                }
                rep.cursors.emplace_back(std::move(*key), *seq);
            }
            auto failure = r.bytes();
            if (!failure) {
                return std::nullopt;
            }
            rep.region = std::move(*name);
            rep.status = static_cast<replica_status>(*status);
            rep.created_ms = *created;
            rep.backfilled = *backfilled;
            rep.failure = std::move(*failure);
            d.replicas.push_back(std::move(rep));
        }
    }
    if (version >= 11) {
        const auto strong = get_flag(r);
        const auto n = r.count();
        if (!strong || !n) {
            return std::nullopt;
        }
        d.strong = *strong;
        for (std::size_t i = 0; i < *n; ++i) {
            auto region = r.bytes();
            const auto status = r.byte();
            if (!region || !status || *status > 2) {
                return std::nullopt;
            }
            d.witnesses.push_back(
                table_witness{std::move(*region), static_cast<witness_status>(*status)});
        }
        const auto m = r.count();
        if (!m) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < *m; ++i) {
            const auto from = r.varint();
            const auto to = r.varint();
            if (!from || !to) {
                return std::nullopt;
            }
            d.retired_ids.emplace_back(*from, *to);
        }
        const auto after = r.varint();
        if (!after) {
            return std::nullopt;
        }
        d.salvage_after_ms = *after;
    }
    d.name = std::move(*name);
    d.table_id = *id;
    d.schema = std::move(*schema);
    d.status = static_cast<table_status>(*status);
    d.created_ms = *created;
    d.billing = static_cast<billing_mode>(*billing);
    d.read_capacity = *rcu;
    d.write_capacity = *wcu;
    d.storage_class = static_cast<table_class>(*cls);
    d.deletion_protection = *protection;
    d.warm_read = *warm_read;
    d.warm_write = *warm_write;
    d.lsis = std::move(*lsis);
    d.gsis = std::move(*gsis);
    d.revision = *revision;
    return d;
}

}  // namespace catalog_detail

/// A descriptor's stored form: a format version, then its fields.
[[nodiscard]] inline auto encode_table_descriptor(const table_descriptor& d) -> std::string {
    std::string out;
    const auto version = catalog_detail::version_for(d);
    out.push_back(static_cast<char>(version));
    catalog_detail::put_descriptor_body(out, d, version);
    return out;
}

[[nodiscard]] inline auto decode_table_descriptor(std::string_view bytes)
    -> std::optional<table_descriptor> {
    if (bytes.empty() || static_cast<std::uint8_t>(bytes[0]) == 0 ||
        static_cast<std::uint8_t>(bytes[0]) > catalog_detail::k_descriptor_format_version) {
        return std::nullopt;
    }
    detail::reader r(bytes.substr(1));
    auto d = catalog_detail::get_descriptor_body(r, static_cast<std::uint8_t>(bytes[0]));
    if (!d || !r.done()) {
        return std::nullopt;
    }
    return d;
}

[[nodiscard]] inline auto encode_delete_job(const delete_job& j) -> std::string {
    std::string out;
    out.push_back(static_cast<char>(catalog_detail::k_job_format_version));
    detail::put_varint(out, j.table_id);
    detail::put_bytes(out, j.name);
    item_sm_detail::put_schema(out, j.schema);
    detail::put_varint(out, j.deleted);
    return out;
}

[[nodiscard]] inline auto decode_delete_job(std::string_view bytes) -> std::optional<delete_job> {
    if (bytes.empty() ||
        static_cast<std::uint8_t>(bytes[0]) != catalog_detail::k_job_format_version) {
        return std::nullopt;
    }
    detail::reader r(bytes.substr(1));
    delete_job j;
    const auto id = r.varint();
    auto name = r.bytes();
    auto schema = item_sm_detail::get_schema(r);
    const auto deleted = r.varint();
    if (!id || !name || !schema || !deleted || !r.done()) {
        return std::nullopt;
    }
    j.table_id = *id;
    j.name = std::move(*name);
    j.schema = std::move(*schema);
    j.deleted = *deleted;
    return j;
}

[[nodiscard]] inline auto encode_catalog_command(const catalog_command& c) -> std::string {
    std::string out;
    // At least the version that brought the command's kind.
    auto version = catalog_detail::version_for(c.table);
    switch (c.op) {
        case catalog_command::kind::seed_ids:
        case catalog_command::kind::make_strong:
        case catalog_command::kind::adopt_strong:
        case catalog_command::kind::strong_members:
        case catalog_command::kind::salvage_done:
            version = std::max<std::uint8_t>(version, 11);
            break;
        case catalog_command::kind::set_replicas:
        case catalog_command::kind::replica_progress:
            version = std::max<std::uint8_t>(version, 10);
            break;
        case catalog_command::kind::set_resource_policy:
        case catalog_command::kind::set_contributor_insights:
            version = std::max<std::uint8_t>(version, 8);
            break;
        case catalog_command::kind::set_pitr:
        case catalog_command::kind::pitr_progress:
            version = std::max<std::uint8_t>(version, 7);
            break;
        case catalog_command::kind::restore_done:
            version = std::max<std::uint8_t>(version, 6);
            break;
        case catalog_command::kind::set_destination:
        case catalog_command::kind::destination_progress:
            version = std::max<std::uint8_t>(version, 5);
            break;
        default:
            break;
    }
    // A vector update can leave the table none (its last index dropped).
    if ((c.update_fields & catalog_command::update_vectors) != 0) {
        version = std::max<std::uint8_t>(version, 9);
    }
    out.push_back(static_cast<char>(version));
    out.push_back(static_cast<char>(c.op));
    catalog_detail::put_descriptor_body(out, c.table, version);
    out.push_back(static_cast<char>(c.update_fields));
    out.push_back(static_cast<char>(c.from));
    out.push_back(static_cast<char>(c.to));
    detail::put_varint(out, c.now_ms);
    detail::put_varint(out, c.deleted);
    out.push_back(c.done ? '\1' : '\0');
    detail::put_varint(out, c.ttl_min_interval_ms);
    return out;
}

/// The command in `bytes`, or nothing when they are not one. A newer format
/// version is reported through `newer`, so the caller can stop the group
/// instead of guessing.
[[nodiscard]] inline auto decode_catalog_command(std::string_view bytes, bool& newer)
    -> std::optional<catalog_command> {
    newer = false;
    if (bytes.size() < 2) {
        return std::nullopt;
    }
    const auto version = static_cast<std::uint8_t>(bytes[0]);
    if (version > catalog_detail::k_command_format_version) {
        newer = true;
        return std::nullopt;
    }
    const auto op = static_cast<std::uint8_t>(bytes[1]);
    // set_ttl came in version 3, the destination commands in 5,
    // restore_done in 6, the point-in-time recovery commands in 7, the
    // resource policy and Contributor Insights commands in 8.
    const std::uint8_t last_op = version >= 11   ? 22
                                 : version >= 10 ? 17
                                 : version >= 8  ? 15
                                 : version >= 7  ? 13
                                 : version >= 6  ? 11
                                 : version >= 5  ? 10
                                 : version >= 3  ? 8
                                                 : 7;
    if (version == 0 || op < 1 || op > last_op) {
        return std::nullopt;
    }
    detail::reader r(bytes.substr(2));
    catalog_command c;
    c.op = static_cast<catalog_command::kind>(op);
    auto table = catalog_detail::get_descriptor_body(r, version);
    const auto fields = r.byte();
    const auto from = r.byte();
    const auto to = r.byte();
    const auto now = r.varint();
    const auto deleted = r.varint();
    const auto done = catalog_detail::get_flag(r);
    const auto interval = version >= 3 ? r.varint() : std::optional<std::uint64_t>{0};
    // update_gsis came with the descriptor's GSI list, in version 2, and
    // update_stream with its streams, in 4.
    // update_vectors with the vector indexes, in 9.
    const std::uint8_t known_fields = version >= 9   ? 0xFF
                                      : version >= 4 ? 0x7F
                                      : version >= 2 ? 0x3F
                                                     : 0x1F;
    if (!table || !fields || *fields > known_fields || !from || *from > 3 || !to || *to > 3 ||
        !now || !deleted || !done || !interval || !r.done()) {
        return std::nullopt;
    }
    c.table = std::move(*table);
    c.update_fields = *fields;
    c.from = static_cast<table_status>(*from);
    c.to = static_cast<table_status>(*to);
    c.now_ms = *now;
    c.deleted = *deleted;
    c.done = *done;
    c.ttl_min_interval_ms = *interval;
    return c;
}

[[nodiscard]] inline auto encode_catalog_result(const catalog_result& r) -> std::string {
    std::string out;
    const auto version = r.table ? catalog_detail::version_for(*r.table) : std::uint8_t{3};
    out.push_back(static_cast<char>(version));
    detail::put_bytes(out, r.error_type);
    detail::put_bytes(out, r.error_message);
    out.push_back(r.table ? '\1' : '\0');
    if (r.table) {
        catalog_detail::put_descriptor_body(out, *r.table, version);
    }
    return out;
}

[[nodiscard]] inline auto decode_catalog_result(std::string_view bytes)
    -> std::optional<catalog_result> {
    if (bytes.empty() || static_cast<std::uint8_t>(bytes[0]) == 0 ||
        static_cast<std::uint8_t>(bytes[0]) > catalog_detail::k_result_format_version) {
        return std::nullopt;
    }
    const auto version = static_cast<std::uint8_t>(bytes[0]);
    detail::reader r(bytes.substr(1));
    catalog_result out;
    auto type = r.bytes();
    auto message = r.bytes();
    const auto has_table = catalog_detail::get_flag(r);
    if (!type || !message || !has_table) {
        return std::nullopt;
    }
    if (*has_table) {
        out.table = catalog_detail::get_descriptor_body(r, version);
        if (!out.table) {
            return std::nullopt;
        }
    }
    if (!r.done()) {
        return std::nullopt;
    }
    out.error_type = std::move(*type);
    out.error_message = std::move(*message);
    return out;
}

/// The catalog group's state machine.
template<kythira::kv_engine Engine = kythira::memory_kv_engine, typename LogIndex = std::uint64_t>
class dynamo_catalog_state_machine {
public:
    using engine_type = Engine;

    /// Opens on the catalog range (`catalog_range()`); any other range is a
    /// host bug.
    auto open(engine_type& engine, const kythira::key_range& range) -> void {
        if (!(range == catalog_range())) {
            throw std::invalid_argument("dynamo_catalog_state_machine: not the catalog range");
        }
        _engine = &engine;
        _ring.clear();
        _applied = 0;
        if (const auto v = _engine->get(kythira::column_family::meta, applied_key())) {
            std::string_view in(*v);
            if (const auto index = item_sm_detail::get_u64(in); index && in.empty()) {
                _applied = static_cast<LogIndex>(*index);
            }
        }
        _durable_floor = _applied;
    }

    auto apply(const std::vector<std::byte>& entry, LogIndex index) -> std::vector<std::byte> {
        if (index <= _applied) {
            return {};
        }
        kythira::write_batch batch;
        auto out = stage(entry, batch);
        commit_staged(batch, index);
        return out;
    }

    // -- persistent_staged_state_machine --------------------------------------
    //
    // The catalog takes part in no transaction, but its group runs the same
    // `transactional_state_machine` wrapper as the item groups (a host runs
    // one state machine type), which drives it through these.

    /// Stages one command's effects into `batch` and returns its result.
    auto stage(const std::vector<std::byte>& entry, kythira::write_batch& batch)
        -> std::vector<std::byte> {
        bool newer = false;
        const auto c = decode_catalog_command(
            std::string_view(reinterpret_cast<const char*>(entry.data()), entry.size()),  // NOLINT
            newer);
        if (!c) {
            if (newer) {
                throw std::runtime_error(
                    "dynamo_catalog_state_machine: an entry uses a newer catalog format; upgrade "
                    "this node");
            }
            throw std::invalid_argument("dynamo_catalog_state_machine: malformed entry");
        }
        catalog_result result;
        switch (c->op) {
            case catalog_command::kind::create_table:
                result = create_table(*c, batch);
                break;
            case catalog_command::kind::set_status:
                result = set_status(*c, batch);
                break;
            case catalog_command::kind::update_table:
                result = update_table(*c, batch);
                break;
            case catalog_command::kind::delete_table:
                result = delete_table(*c, batch);
                break;
            case catalog_command::kind::job_progress:
                result = job_progress(*c, batch);
                break;
            case catalog_command::kind::tag_resource:
            case catalog_command::kind::untag_resource:
                result = retag(*c, batch);
                break;
            case catalog_command::kind::set_ttl:
                result = set_ttl(*c, batch);
                break;
            case catalog_command::kind::set_destination:
                result = set_destination(*c, batch);
                break;
            case catalog_command::kind::destination_progress:
                result = destination_progress(*c, batch);
                break;
            case catalog_command::kind::restore_done:
                result = restore_done(*c, batch);
                break;
            case catalog_command::kind::set_pitr:
                result = set_pitr(*c, batch);
                break;
            case catalog_command::kind::pitr_progress:
                result = pitr_progress(*c, batch);
                break;
            case catalog_command::kind::set_resource_policy:
                result = set_resource_policy(*c, batch);
                break;
            case catalog_command::kind::set_contributor_insights:
                result = set_contributor_insights(*c, batch);
                break;
            case catalog_command::kind::set_replicas:
                result = set_replicas(*c, batch);
                break;
            case catalog_command::kind::replica_progress:
                result = replica_progress(*c, batch);
                break;
            case catalog_command::kind::seed_ids:
                if (next_table_id() < c->table.table_id) {
                    batch.put(kythira::column_family::data, catalog_detail::next_id_key(),
                              catalog_detail::u64_string(c->table.table_id));
                }
                break;
            case catalog_command::kind::make_strong:
                result = make_strong(*c, batch);
                break;
            case catalog_command::kind::adopt_strong:
                result = adopt_strong(*c, batch);
                break;
            case catalog_command::kind::strong_members:
                result = strong_members(*c, batch);
                break;
            case catalog_command::kind::salvage_done:
                result = salvage_done(*c, batch);
                break;
        }
        const auto out = encode_catalog_result(result);
        std::vector<std::byte> bytes(out.size());
        std::memcpy(bytes.data(), out.data(), out.size());
        return bytes;
    }

    /// Writes `batch` with the applied index `index`.
    auto commit_staged(kythira::write_batch& batch, LogIndex index) -> void {
        batch.put(kythira::column_family::meta, applied_key(),
                  catalog_detail::u64_string(static_cast<std::uint64_t>(index)));
        const auto seq = engine().write(batch);
        _applied = index;
        _ring.emplace_back(index, seq);
        trim_ring();
    }

    /// Nothing is kept between `stage` and `commit_staged`.
    auto discard_staged() -> void {}

    [[nodiscard]] auto key_prefix() const -> const std::string& {
        static const std::string none;
        return none;
    }

    [[nodiscard]] auto applied_index() const -> LogIndex { return _applied; }

    [[nodiscard]] auto durable_applied_index() const -> LogIndex {
        const auto durable = engine().durable_sequence();
        LogIndex best = _durable_floor;
        for (const auto& [index, seq] : _ring) {
            if (seq > durable) {
                break;
            }
            best = index;
        }
        return best;
    }

    auto checkpoint(const std::filesystem::path& dir) -> kythira::snapshot_manifest {
        return engine().checkpoint(catalog_range(), dir);
    }

    auto ingest(const std::filesystem::path& dir, const kythira::snapshot_manifest& manifest,
                LogIndex index) -> void {
        engine().ingest(catalog_range(), dir, manifest);
        _applied = index;
        _durable_floor = index;
        _ring.clear();
    }

    auto drop() -> void { engine().drop(catalog_range()); }

    // -- Reads ----------------------------------------------------------------

    [[nodiscard]] auto describe(std::string_view name) const -> std::optional<table_descriptor> {
        const auto v = engine().get(kythira::column_family::data, catalog_detail::table_key(name));
        if (!v) {
            return std::nullopt;
        }
        auto d = decode_table_descriptor(*v);
        if (!d) {
            throw kythira::persistent_storage_error("dynamo_catalog_state_machine: corrupt table");
        }
        return d;
    }

    /// The table with id `table_id`, or nothing. Reads every descriptor;
    /// callers cache the answer.
    [[nodiscard]] auto describe_id(std::uint64_t table_id) const
        -> std::optional<table_descriptor> {
        std::optional<table_descriptor> out;
        engine().scan(kythira::column_family::data, catalog_detail::tables_range(),
                      kythira::scan_direction::forward, [&](std::string_view, std::string_view v) {
                          auto d = decode_table_descriptor(v);
                          if (!d) {
                              throw kythira::persistent_storage_error(
                                  "dynamo_catalog_state_machine: corrupt table");
                          }
                          if (d->table_id == table_id) {
                              out = std::move(d);
                              return false;
                          }
                          return true;
                      });
        return out;
    }

    /// Table names in order, after `exclusive_start` (`ListTables`),
    /// at most `limit` of them.
    [[nodiscard]] auto list_tables(std::string_view exclusive_start, std::size_t limit) const
        -> std::vector<std::string> {
        std::vector<std::string> out;
        auto range = catalog_detail::tables_range();
        const auto prefix_size = range.start.size();
        if (!exclusive_start.empty()) {
            // The smallest key after `table/<start>`.
            range.start = catalog_detail::table_key(exclusive_start) + std::string(1, '\0');
        }
        engine().scan(kythira::column_family::data, range, kythira::scan_direction::forward,
                      [&](std::string_view k, std::string_view) {
                          if (out.size() == limit) {
                              return false;
                          }
                          out.emplace_back(k.substr(prefix_size));
                          return true;
                      });
        return out;
    }

    /// Delete jobs in progress, oldest first: what the catalog leader drives.
    [[nodiscard]] auto delete_jobs() const -> std::vector<delete_job> {
        std::vector<delete_job> out;
        engine().scan(kythira::column_family::data, catalog_detail::jobs_range(),
                      kythira::scan_direction::forward, [&](std::string_view, std::string_view v) {
                          auto j = decode_delete_job(v);
                          if (!j) {
                              throw kythira::persistent_storage_error(
                                  "dynamo_catalog_state_machine: corrupt job");
                          }
                          out.push_back(std::move(*j));
                          return true;
                      });
        return out;
    }

    /// The id the next table will get.
    [[nodiscard]] auto next_table_id() const -> std::uint64_t {
        const auto v = engine().get(kythira::column_family::data, catalog_detail::next_id_key());
        if (!v) {
            return 1;
        }
        std::string_view in(*v);
        const auto id = item_sm_detail::get_u64(in);
        if (!id || !in.empty()) {
            throw kythira::persistent_storage_error("dynamo_catalog_state_machine: corrupt id");
        }
        return *id;
    }

private:
    [[nodiscard]] auto engine() const -> engine_type& {
        if (_engine == nullptr) {
            throw std::logic_error("dynamo_catalog_state_machine: not opened");
        }
        return *_engine;
    }

    [[nodiscard]] static auto applied_key() -> const std::string& {
        static const std::string key = catalog_range().start;
        return key;
    }

    static auto failure(std::string_view type, std::string message) -> catalog_result {
        catalog_result r;
        r.error_type = dynamodb_error_style().service_prefix + std::string(type);
        r.error_message = std::move(message);
        return r;
    }

    static auto not_found(std::string_view name) -> catalog_result {
        return failure("ResourceNotFoundException",
                       "Requested resource not found: Table: " + std::string(name) + " not found");
    }

    static auto in_use(const table_descriptor& d) -> catalog_result {
        std::string_view doing = "updated";
        if (d.status == table_status::creating) {
            doing = "created";
        } else if (d.status == table_status::deleting) {
            doing = "deleted";
        }
        return failure("ResourceInUseException",
                       "Attempt to change a resource which is still in use: Table is being " +
                           std::string(doing) + ": " + d.name);
    }

    static auto store(kythira::write_batch& batch, const table_descriptor& d) -> void {
        batch.put(kythira::column_family::data, catalog_detail::table_key(d.name),
                  encode_table_descriptor(d));
    }

    auto create_table(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        if (describe(c.table.name)) {
            return failure("ResourceInUseException", "Table already exists: " + c.table.name);
        }
        table_descriptor d = c.table;
        // One value per tag key, in key order, as TagResource keeps them.
        const std::map<std::string, std::string> tags(d.tags.begin(), d.tags.end());
        d.tags.assign(tags.begin(), tags.end());
        d.table_id = next_table_id();
        std::uint64_t next = d.table_id + 1;
        for (auto& g : d.gsis) {
            g.index_id = next++;
            g.status = index_status::active;
        }
        // An empty table's vector graphs are built at once (a restore's
        // from its items as they load); a backup's dropped index stays so.
        std::erase_if(d.vectors,
                      [](const vector_index& v) { return v.status == index_status::deleting; });
        for (auto& v : d.vectors) {
            v.status = index_status::active;
            v.created_ms = c.now_ms;
        }
        d.status = table_status::creating;
        d.created_ms = c.now_ms;
        d.revision = 1;
        // A stream asked for at creation is enabled with the table.
        std::erase_if(d.streams, [](const table_stream& s) { return s.disabled_ms != 0; });
        if (!d.streams.empty()) {
            d.streams = {table_stream{c.now_ms, d.streams.front().view, 0}};
        }
        // A restore's table loads its items before it becomes ACTIVE.
        d.restore.in_progress = d.restore.restored();
        if (d.restore.in_progress) {
            d.restore.restore_ms = c.now_ms;
        }
        // Point-in-time recovery is a setting of the source, never copied;
        // nor are Contributor Insights or a restored table's policies. A
        // `CreateTable` `ResourcePolicy` is the table's first revision.
        d.pitr = {};
        d.insights.clear();
        // A restore is never global (task 22); a replica a peer creates
        // starts with its region, its peers and no replicator progress.
        if (d.restore.restored()) {
            d.region.clear();
            d.replicas.clear();
        }
        for (auto& r : d.replicas) {
            r.cursors.clear();
            r.created_ms = c.now_ms;
        }
        if (d.restore.restored()) {
            d.policies.clear();
        }
        std::erase_if(d.policies, [](const resource_policy& p) { return p.stream_label != 0; });
        for (auto& p : d.policies) {
            p.revision = std::to_string(c.now_ms);
        }
        batch.put(kythira::column_family::data, catalog_detail::next_id_key(),
                  catalog_detail::u64_string(next));
        store(batch, d);
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    auto set_status(const catalog_command& c, kythira::write_batch& batch) const -> catalog_result {
        auto d = describe(c.table.name);
        if (!d || d->table_id != c.table.table_id) {
            return not_found(c.table.name);
        }
        // A transition from a status the table has left is a stale or
        // repeated entry: report the table as it is, change nothing.
        if (d->status == c.from && c.from != c.to) {
            d->status = c.to;
            ++d->revision;
            store(batch, *d);
        }
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    auto update_table(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        auto d = describe(c.table.name);
        if (!d) {
            return not_found(c.table.name);
        }
        if (d->status != table_status::active) {
            return in_use(*d);
        }
        bool needs_work = false;
        if ((c.update_fields & catalog_command::update_billing) != 0 &&
            d->billing != c.table.billing) {
            d->billing = c.table.billing;
            needs_work = true;
        }
        if ((c.update_fields & catalog_command::update_throughput) != 0 &&
            (d->read_capacity != c.table.read_capacity ||
             d->write_capacity != c.table.write_capacity)) {
            d->read_capacity = c.table.read_capacity;
            d->write_capacity = c.table.write_capacity;
            needs_work = true;
        }
        if ((c.update_fields & catalog_command::update_class) != 0 &&
            d->storage_class != c.table.storage_class) {
            d->storage_class = c.table.storage_class;
            needs_work = true;
        }
        if ((c.update_fields & catalog_command::update_warm_throughput) != 0 &&
            (d->warm_read != c.table.warm_read || d->warm_write != c.table.warm_write)) {
            d->warm_read = c.table.warm_read;
            d->warm_write = c.table.warm_write;
            needs_work = true;
        }
        if ((c.update_fields & catalog_command::update_gsis) != 0) {
            if (auto refused = update_gsis(c, *d, batch)) {
                return *refused;
            }
        }
        if ((c.update_fields & catalog_command::update_stream) != 0) {
            if (auto refused = update_stream(c, *d)) {
                return *refused;
            }
        }
        if ((c.update_fields & catalog_command::update_vectors) != 0) {
            if (auto refused = update_vectors(c, *d)) {
                return *refused;
            }
        }
        if ((c.update_fields & catalog_command::update_deletion_protection) != 0) {
            // Takes effect at once and needs no background work, so the
            // table stays ACTIVE.
            d->deletion_protection = c.table.deletion_protection;
        }
        if (d->billing == billing_mode::pay_per_request) {
            d->read_capacity = 0;
            d->write_capacity = 0;
        }
        if (needs_work) {
            d->status = table_status::updating;
        }
        ++d->revision;
        store(batch, *d);
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    /// `update_stream` (see `catalog_command::update_field`) applied to
    /// `d`, refused in DynamoDB's words. Takes effect at once, as on AWS: a
    /// stream needs no background work, so the table stays ACTIVE.
    /// Disabled streams whose records have all expired stop being listed.
    static auto update_stream(const catalog_command& c, table_descriptor& d)
        -> std::optional<catalog_result> {
        if (c.table.streams.size() != 1) {
            return invalid("Stream StreamEnabled was null");
        }
        const auto& want = c.table.streams.front();
        if (want.disabled_ms == 0) {
            if (d.enabled_stream() != nullptr) {
                return invalid("Table already has an enabled stream");
            }
            // Labels are unique per table, even for two enables in one ms.
            const auto label =
                std::max(c.now_ms, d.streams.empty() ? 0 : d.streams.back().label_ms + 1);
            d.streams.push_back(table_stream{label, want.view, 0});
        } else {
            if (d.enabled_stream() == nullptr) {
                return invalid("Table has no stream to disable");
            }
            d.streams.back().disabled_ms = std::max<std::uint64_t>(c.now_ms, 1);
        }
        std::erase_if(d.streams, [&](const table_stream& s) {
            return s.disabled_ms != 0 && s.disabled_ms + k_stream_retention_ms < c.now_ms;
        });
        return std::nullopt;
    }

    /// `update_vectors` (see `catalog_command::update_field`) applied to
    /// `d`, refused in DynamoDB's words.
    static auto update_vectors(const catalog_command& c, table_descriptor& d)
        -> std::optional<catalog_result> {
        const auto named = [](const std::vector<vector_index>& list,
                              const std::string& name) -> const vector_index* {
            const auto it = std::find_if(list.begin(), list.end(),
                                         [&](const vector_index& v) { return v.name == name; });
            return it == list.end() ? nullptr : &*it;
        };
        std::vector<vector_index> added;
        std::size_t removed = 0;
        for (const auto& v : c.table.vectors) {
            if (named(d.vectors, v.name) == nullptr) {
                added.push_back(v);
            }
        }
        for (const auto& v : d.vectors) {
            if (v.status == index_status::active && named(c.table.vectors, v.name) == nullptr) {
                ++removed;
            }
        }
        const bool busy =
            std::any_of(d.vectors.begin(), d.vectors.end(),
                        [](const vector_index& v) { return v.status != index_status::active; });
        if (added.size() + removed > 1 || (busy && added.size() + removed == 1)) {
            return failure("LimitExceededException",
                           "Subscriber limit exceeded: Only 1 online index can be created or "
                           "deleted simultaneously per table");
        }
        if (!added.empty() && d.vectors.size() >= k_max_vector_indexes) {
            return failure("LimitExceededException",
                           "VectorIndex count exceeds the per-table limit");
        }
        if (!added.empty() &&
            (named(d.vectors, added.front().name) != nullptr ||
             std::any_of(d.gsis.begin(), d.gsis.end(),
                         [&](const global_index& g) { return g.name == added.front().name; }) ||
             std::any_of(d.lsis.begin(), d.lsis.end(),
                         [&](const local_index& l) { return l.name == added.front().name; }))) {
            return invalid("Index already exists: " + added.front().name);
        }
        std::erase_if(d.vectors, [&](const vector_index& v) {
            return v.status == index_status::deleting && named(c.table.vectors, v.name) == nullptr;
        });
        for (auto& v : d.vectors) {
            const auto* want = named(c.table.vectors, v.name);
            if (want == nullptr) {
                if (v.status == index_status::active) {
                    v.status = index_status::deleting;
                }
                continue;
            }
            if (v.status == index_status::creating && want->status == index_status::active) {
                v.status = index_status::active;
            }
        }
        if (!added.empty()) {
            auto v = added.front();
            v.status = index_status::creating;
            v.created_ms = c.now_ms;
            d.vectors.push_back(std::move(v));
        }
        return std::nullopt;
    }

    /// `update_gsis` (see `catalog_command::update_field`) applied to `d`.
    /// Returns the refusal, if any; on success `d` holds the new list.
    auto update_gsis(const catalog_command& c, table_descriptor& d,
                     kythira::write_batch& batch) const -> std::optional<catalog_result> {
        const auto named = [](const std::vector<global_index>& list,
                              const std::string& name) -> const global_index* {
            const auto it = std::find_if(list.begin(), list.end(),
                                         [&](const global_index& g) { return g.name == name; });
            return it == list.end() ? nullptr : &*it;
        };
        std::vector<global_index> added;
        std::vector<std::string> removed;
        for (const auto& g : c.table.gsis) {
            if (named(d.gsis, g.name) == nullptr) {
                added.push_back(g);
            }
        }
        for (const auto& g : d.gsis) {
            if (named(c.table.gsis, g.name) == nullptr) {
                removed.push_back(g.name);
            }
        }
        const bool busy = std::any_of(d.gsis.begin(), d.gsis.end(), [](const global_index& g) {
            return g.status != index_status::active;
        });
        if (added.size() + removed.size() > 1 || (busy && added.size() + removed.size() == 1)) {
            return failure("LimitExceededException",
                           "Subscriber limit exceeded: Only 1 online index can be created or "
                           "deleted simultaneously per table");
        }
        if (!added.empty() && d.gsis.size() >= k_max_global_indexes) {
            return failure("LimitExceededException",
                           "GlobalSecondaryIndex count exceeds the per-table limit");
        }
        for (auto& g : d.gsis) {
            const auto* want = named(c.table.gsis, g.name);
            if (want == nullptr) {
                if (g.status == index_status::active) {
                    g.status = index_status::deleting;
                    std::erase_if(d.insights, [&](const contributor_insights& ci) {
                        return ci.index == g.name;
                    });
                    batch.put(kythira::column_family::data, catalog_detail::job_key(g.index_id),
                              encode_delete_job(delete_job{g.index_id, d.name, g.schema, 0}));
                }
                continue;
            }
            if (g.status == index_status::creating && want->status == index_status::active) {
                g.status = index_status::active;
            }
            if (g.has_throughput && want->has_throughput) {
                g.read_capacity = want->read_capacity;
                g.write_capacity = want->write_capacity;
            }
        }
        if (!added.empty()) {
            auto g = added.front();
            const auto id = next_table_id();
            g.index_id = id;
            g.status = index_status::creating;
            batch.put(kythira::column_family::data, catalog_detail::next_id_key(),
                      catalog_detail::u64_string(id + 1));
            d.gsis.push_back(std::move(g));
        }
        return std::nullopt;
    }

    auto delete_table(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        auto d = describe(c.table.name);
        if (!d) {
            return not_found(c.table.name);
        }
        if (d->status != table_status::active) {
            return in_use(*d);
        }
        if (d->deletion_protection) {
            catalog_result r;
            r.error_type = dynamodb_error_style().validation_type;
            r.error_message =
                "Resource cannot be deleted as it is currently protected against deletion. "
                "Disable deletion protection first.";
            return r;
        }
        d->status = table_status::deleting;
        ++d->revision;
        store(batch, *d);
        batch.put(kythira::column_family::data, catalog_detail::job_key(d->table_id),
                  encode_delete_job(delete_job{d->table_id, d->name, d->schema, 0}));
        for (const auto& g : d->gsis) {
            batch.put(kythira::column_family::data, catalog_detail::job_key(g.index_id),
                      encode_delete_job(delete_job{g.index_id, d->name, g.schema, 0}));
        }
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    auto job_progress(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        const auto key = catalog_detail::job_key(c.table.table_id);
        const auto v = engine().get(kythira::column_family::data, key);
        if (!v) {
            // Already finished (a repeated last step).
            return {};
        }
        auto job = decode_delete_job(*v);
        if (!job) {
            throw kythira::persistent_storage_error("dynamo_catalog_state_machine: corrupt job");
        }
        job->deleted += c.deleted;
        catalog_result r;
        if (c.done) {
            batch.erase(kythira::column_family::data, key);
            auto d = describe(job->name);
            if (d && d->table_id == job->table_id) {
                batch.erase(kythira::column_family::data, catalog_detail::table_key(job->name));
            } else if (d) {
                // A deleted index's key space is empty: the index goes.
                const auto n = std::erase_if(d->gsis, [&](const global_index& g) {
                    return g.index_id == job->table_id && g.status == index_status::deleting;
                });
                if (n > 0) {
                    ++d->revision;
                    store(batch, *d);
                }
            }
            return r;
        }
        batch.put(kythira::column_family::data, key, encode_delete_job(*job));
        r.table = describe(job->name);
        return r;
    }

    /// Tags in key order. AWS keeps one value per key; a deleting table
    /// cannot be tagged.
    auto retag(const catalog_command& c, kythira::write_batch& batch) const -> catalog_result {
        auto d = describe(c.table.name);
        if (!d || (c.table.table_id != 0 && d->table_id != c.table.table_id)) {
            return not_found(c.table.name);
        }
        if (d->status == table_status::deleting) {
            return in_use(*d);
        }
        std::map<std::string, std::string> tags(d->tags.begin(), d->tags.end());
        for (const auto& [k, v] : c.table.tags) {
            if (c.op == catalog_command::kind::tag_resource) {
                tags[k] = v;
            } else {
                tags.erase(k);
            }
        }
        d->tags.assign(tags.begin(), tags.end());
        ++d->revision;
        store(batch, *d);
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    /// `UpdateTimeToLive`, with DynamoDB's refusals in its words. The
    /// checks run in DynamoDB Local's order: the state first, then the
    /// interval.
    auto set_ttl(const catalog_command& c, kythira::write_batch& batch) const -> catalog_result {
        auto d = describe(c.table.name);
        if (!d) {
            return not_found(c.table.name);
        }
        if (d->status == table_status::deleting) {
            return in_use(*d);
        }
        const auto& want = c.table.ttl;
        if (d->ttl.enabled && want.attribute != d->ttl.attribute) {
            return invalid("TimeToLive is active on a different AttributeName");
        }
        if (d->ttl.enabled == want.enabled) {
            return invalid(want.enabled ? "TimeToLive is already enabled"
                                        : "TimeToLive is already disabled");
        }
        if (c.ttl_min_interval_ms != 0 && d->ttl.changed_ms != 0 &&
            c.now_ms < d->ttl.changed_ms + c.ttl_min_interval_ms) {
            return invalid("Time to live has been modified multiple times within a fixed interval");
        }
        d->ttl.enabled = want.enabled;
        d->ttl.attribute = want.enabled ? want.attribute : std::string{};
        d->ttl.changed_ms = c.now_ms;
        ++d->revision;
        store(batch, *d);
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    /// `set_destination` (task 17). AWS's refusals are matched in kind
    /// (`ValidationException`, `ResourceInUseException`); their wording
    /// is not recorded from AWS (DynamoDB Local has no Kinesis
    /// destinations), so it is Kythira's.
    auto set_destination(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        auto d = describe(c.table.name);
        if (!d) {
            return not_found(c.table.name);
        }
        if (c.table.destinations.size() != 1) {
            return invalid("One Kinesis streaming destination is required");
        }
        if (d->status != table_status::active) {
            return in_use(*d);
        }
        const auto& want = c.table.destinations.front();
        const auto it = std::find_if(
            d->destinations.begin(), d->destinations.end(),
            [&](const kinesis_destination& k) { return k.stream_arn == want.stream_arn; });
        switch (want.status) {
            case destination_status::enabling: {
                if (const auto* on = d->streaming_destination()) {
                    return invalid(on->stream_arn == want.stream_arn
                                       ? "Table is already streaming to this Kinesis data stream"
                                       : "Table can stream to only one Kinesis data stream");
                }
                kinesis_destination k;
                k.stream_arn = want.stream_arn;
                k.status = destination_status::enabling;
                k.precision = want.precision;
                k.enabled_ms = c.now_ms;
                if (it != d->destinations.end()) {
                    *it = std::move(k);
                } else {
                    d->destinations.push_back(std::move(k));
                }
                break;
            }
            case destination_status::disabling:
                if (it == d->destinations.end() || !it->streaming()) {
                    return invalid("Table is not streaming to the Kinesis data stream " +
                                   want.stream_arn);
                }
                it->status = destination_status::disabling;
                break;
            case destination_status::updating:
                if (it == d->destinations.end() || !it->streaming()) {
                    return invalid("Table is not streaming to the Kinesis data stream " +
                                   want.stream_arn);
                }
                if (it->status != destination_status::active) {
                    return failure("ResourceInUseException",
                                   "The Kinesis streaming destination is " +
                                       std::string(it->status == destination_status::enabling
                                                       ? "being enabled"
                                                       : "being updated"));
                }
                if (it->precision == want.precision) {
                    return invalid("ApproximateCreationDateTimePrecision is already " +
                                   std::string(want.precision == time_precision::millisecond
                                                   ? "MILLISECOND"
                                                   : "MICROSECOND"));
                }
                it->status = destination_status::updating;
                it->precision = want.precision;
                break;
            default:
                return invalid("Not a Kinesis streaming destination request");
        }
        ++d->revision;
        store(batch, *d);
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    /// `destination_progress` (task 17): the publisher's status moves and
    /// cursors. A move the destination has left behind (a disable that
    /// came first, say) is ignored, not refused, as is progress for a
    /// destination that stopped streaming.
    auto destination_progress(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        auto d = describe(c.table.name);
        if (!d || c.table.destinations.size() != 1) {
            return d ? invalid("One Kinesis streaming destination is required")
                     : not_found(c.table.name);
        }
        const auto& got = c.table.destinations.front();
        const auto it = std::find_if(
            d->destinations.begin(), d->destinations.end(),
            [&](const kinesis_destination& k) { return k.stream_arn == got.stream_arn; });
        catalog_result r;
        if (it == d->destinations.end() || it->enabled_ms != got.enabled_ms) {
            r.table = std::move(d);
            return r;
        }
        using ds = destination_status;
        const auto from = it->status;
        const bool moves = (from == ds::enabling &&
                            (got.status == ds::active || got.status == ds::enable_failed)) ||
                           (from == ds::updating && got.status == ds::active) ||
                           (from == ds::disabling && got.status == ds::disabled);
        if (!moves && got.status != from) {
            r.table = std::move(d);
            return r;
        }
        it->status = got.status;
        if (got.status == ds::enable_failed) {
            it->status_description = got.status_description;
        }
        if (!it->streaming()) {
            it->cursors.clear();
        } else {
            std::vector<std::pair<std::string, std::uint64_t>> merged;
            merged.reserve(got.cursors.size());
            for (const auto& [key, seq] : got.cursors) {
                auto next = seq;
                for (const auto& [old_key, old_seq] : it->cursors) {
                    if (old_key == key) {
                        next = std::max(next, old_seq);
                    }
                }
                merged.emplace_back(key, next);
            }
            it->cursors = std::move(merged);
        }
        ++d->revision;
        store(batch, *d);
        r.table = std::move(d);
        return r;
    }

    /// `restore_done` (task 18): the restored table's items are loaded. A
    /// repeat, or an entry for a table since replaced, changes nothing.
    auto restore_done(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        auto d = describe(c.table.name);
        if (!d || d->table_id != c.table.table_id) {
            return not_found(c.table.name);
        }
        if (d->restore.in_progress) {
            d->restore.in_progress = false;
            ++d->revision;
            store(batch, *d);
        }
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    /// `set_pitr` (task 19). A fresh enable starts a new archive named by
    /// the entry's time; a disable drops the archive's state (the worker
    /// removes its objects).
    auto set_pitr(const catalog_command& c, kythira::write_batch& batch) const -> catalog_result {
        auto d = describe(c.table.name);
        if (!d) {
            return not_found(c.table.name);
        }
        const auto& got = c.table.pitr;
        if (got.recovery_days < 1 || got.recovery_days > 35) {
            return invalid("RecoveryPeriodInDays must be between 1 and 35");
        }
        if (got.enabled && d->pitr.enabled) {
            d->pitr.recovery_days = got.recovery_days;
        } else if (got.enabled) {
            d->pitr = continuous_backups{};
            d->pitr.enabled = true;
            d->pitr.recovery_days = got.recovery_days;
            d->pitr.enabled_ms = c.now_ms;
        } else {
            d->pitr = continuous_backups{};
        }
        ++d->revision;
        store(batch, *d);
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    /// `pitr_progress` (task 19): see the command.
    auto pitr_progress(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        auto d = describe(c.table.name);
        if (!d) {
            return not_found(c.table.name);
        }
        const auto& got = c.table.pitr;
        catalog_result r;
        if (!d->pitr.enabled || d->pitr.enabled_ms != got.enabled_ms) {
            r.table = std::move(d);
            return r;
        }
        auto& p = d->pitr;
        std::vector<std::pair<std::string, std::uint64_t>> merged;
        merged.reserve(got.cursors.size());
        for (const auto& [key, seq] : got.cursors) {
            auto next = seq;
            for (const auto& [old_key, old_seq] : p.cursors) {
                if (old_key == key) {
                    next = std::max(next, old_seq);
                }
            }
            merged.emplace_back(key, next);
        }
        p.cursors = std::move(merged);
        p.next_object = std::max(p.next_object, got.next_object);
        p.latest_ms = std::max(p.latest_ms, got.latest_ms);
        p.earliest_ms = std::max(p.earliest_ms, got.earliest_ms);
        ++d->revision;
        store(batch, *d);
        r.table = std::move(d);
        return r;
    }

    /// `set_resource_policy` (task 21): see the command.
    auto set_resource_policy(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        auto d = describe(c.table.name);
        if (!d || d->status == table_status::deleting) {
            return not_found(c.table.name);
        }
        if (c.table.policies.size() != 1) {
            return invalid("set_resource_policy needs exactly one policy");
        }
        const auto& want = c.table.policies.front();
        if (want.stream_label != 0 &&
            std::none_of(d->streams.begin(), d->streams.end(),
                         [&](const table_stream& s) { return s.label_ms == want.stream_label; })) {
            return failure("ResourceNotFoundException", "Requested resource not found");
        }
        if (d->status == table_status::creating) {
            return failure("ResourceInUseException",
                           "Attempt to change a resource which is still in use: Table is being "
                           "created: " +
                               d->name);
        }
        const auto it = std::find_if(
            d->policies.begin(), d->policies.end(),
            [&](const resource_policy& p) { return p.stream_label == want.stream_label; });
        const bool has = it != d->policies.end();
        if (!want.revision.empty()) {
            const bool matches =
                want.revision == "NO_POLICY" ? !has : has && it->revision == want.revision;
            if (!matches) {
                return failure("PolicyNotFoundException",
                               has ? "revision " + it->revision : std::string("no policy"));
            }
        }
        catalog_result r;
        if (want.document.empty()) {
            if (has) {
                d->policies.erase(it);
                ++d->revision;
                store(batch, *d);
            }
        } else if (!has || it->document != want.document) {
            // Revisions grow even when two changes share a millisecond.
            std::uint64_t next = c.now_ms;
            if (has) {
                next = std::max<std::uint64_t>(next, std::stoull(it->revision) + 1);
                it->document = want.document;
                it->revision = std::to_string(next);
            } else {
                d->policies.push_back(
                    resource_policy{want.stream_label, want.document, std::to_string(next)});
            }
            ++d->revision;
            store(batch, *d);
        }
        r.table = std::move(d);
        return r;
    }

    /// `set_contributor_insights` (task 21): see the command.
    auto set_contributor_insights(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        auto d = describe(c.table.name);
        if (!d || d->status == table_status::deleting) {
            return not_found(c.table.name);
        }
        if (c.table.insights.size() != 1) {
            return invalid("set_contributor_insights needs exactly one setting");
        }
        const auto& want = c.table.insights.front();
        if (!want.index.empty() &&
            std::none_of(d->gsis.begin(), d->gsis.end(), [&](const global_index& g) {
                return g.name == want.index && g.status != index_status::deleting;
            })) {
            return failure("ResourceNotFoundException",
                           "Requested resource not found: Index: " + want.index + " not found");
        }
        auto it =
            std::find_if(d->insights.begin(), d->insights.end(),
                         [&](const contributor_insights& ci) { return ci.index == want.index; });
        if (it == d->insights.end()) {
            d->insights.push_back(contributor_insights{want.index});
            it = std::prev(d->insights.end());
        }
        it->enabled = want.enabled;
        it->mode = want.mode;
        it->updated_ms = c.now_ms;
        ++d->revision;
        store(batch, *d);
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    /// `set_replicas` (task 22): see the command.
    /// `make_strong` (task 27): see the command.
    auto make_strong(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        auto d = describe(c.table.name);
        if (!d || d->status == table_status::deleting) {
            return not_found(c.table.name);
        }
        if (d->status != table_status::active) {
            return in_use(*d);
        }
        if (d->global() || d->strong) {
            return failure("ValidationException",
                           "Table " + d->name + " is already a global table");
        }
        // The handler read the table's ids before the strong catalog gave
        // the new ones; a table deleted and made again since has others.
        if (c.table.retired_ids.empty() || c.table.retired_ids.front().first != d->table_id ||
            c.table.gsis.size() != d->gsis.size()) {
            return failure("ResourceInUseException",
                           "Attempt to change a resource which is still in use: Table " + d->name +
                               " changed while it was being made strong");
        }
        d->table_id = c.table.table_id;
        for (auto& g : d->gsis) {
            const auto it = std::find_if(c.table.gsis.begin(), c.table.gsis.end(),
                                         [&](const global_index& w) { return w.name == g.name; });
            if (it == c.table.gsis.end()) {
                return failure("ResourceInUseException",
                               "Attempt to change a resource which is still in use: Table " +
                                   d->name + " changed while it was being made strong");
            }
            g.index_id = it->index_id;
        }
        d->strong = true;
        d->region = c.table.region;
        d->replicas = c.table.replicas;
        for (auto& r : d->replicas) {
            r.created_ms = c.now_ms;
            r.cursors.clear();
        }
        d->witnesses = c.table.witnesses;
        d->retired_ids = c.table.retired_ids;
        d->salvage_after_ms = c.table.salvage_after_ms;
        ++d->revision;
        store(batch, *d);
        catalog_result r;
        r.table = std::move(*d);
        return r;
    }

    /// `adopt_strong` (task 27): see the command.
    auto adopt_strong(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        if (describe(c.table.name)) {
            return failure("ResourceInUseException", "Table already exists: " + c.table.name);
        }
        if (!c.table.strong || !is_strong_table(c.table.table_id)) {
            return failure("ValidationException",
                           "Table " + c.table.name + " is not a strongly consistent replica");
        }
        auto d = c.table;
        d.status = table_status::active;
        d.revision = 1;
        d.retired_ids.clear();
        d.salvage_after_ms = 0;
        store(batch, d);
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    /// `strong_members` (task 27): see the command.
    auto strong_members(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        auto d = describe(c.table.name);
        if (!d || d->status == table_status::deleting) {
            return not_found(c.table.name);
        }
        if (!d->strong || d->table_id != c.table.table_id) {
            return failure("ValidationException",
                           "Table " + c.table.name + " is not this strongly consistent table");
        }
        bool changed = false;
        for (const auto& want : c.table.replicas) {
            for (auto& have : d->replicas) {
                if (have.region == want.region &&
                    (have.status != want.status || have.failure != want.failure)) {
                    have.status = want.status;
                    have.failure = want.failure;
                    changed = true;
                }
            }
        }
        for (const auto& want : c.table.witnesses) {
            for (auto& have : d->witnesses) {
                if (have.region == want.region && have.status != want.status) {
                    have.status = want.status;
                    changed = true;
                }
            }
        }
        if (changed) {
            ++d->revision;
            store(batch, *d);
        }
        catalog_result r;
        r.table = std::move(*d);
        return r;
    }

    /// `salvage_done` (task 27): see the command.
    auto salvage_done(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        auto d = describe(c.table.name);
        if (!d || d->table_id != c.table.table_id) {
            return not_found(c.table.name);
        }
        for (const auto& [from, to] : d->retired_ids) {
            // The old key space had the schema of what replaced it.
            auto schema = d->schema;
            for (const auto& g : d->gsis) {
                if (g.index_id == to) {
                    schema = g.schema;
                }
            }
            batch.put(kythira::column_family::data, catalog_detail::job_key(from),
                      encode_delete_job(delete_job{from, d->name, schema, 0}));
        }
        d->retired_ids.clear();
        d->salvage_after_ms = 0;
        store(batch, *d);
        catalog_result r;
        r.table = std::move(*d);
        return r;
    }

    auto set_replicas(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        auto d = describe(c.table.name);
        if (!d || d->status == table_status::deleting) {
            return not_found(c.table.name);
        }
        if (d->status != table_status::active) {
            return in_use(*d);
        }
        const auto named = [](const std::vector<table_replica>& list,
                              const std::string& region) -> const table_replica* {
            const auto it = std::find_if(list.begin(), list.end(), [&](const table_replica& r) {
                return r.region == region;
            });
            return it == list.end() ? nullptr : &*it;
        };
        std::vector<table_replica> added;
        std::size_t removed = 0;
        for (const auto& want : c.table.replicas) {
            if (named(d->replicas, want.region) == nullptr) {
                added.push_back(want);
            }
        }
        for (const auto& have : d->replicas) {
            if (have.status != replica_status::deleting &&
                named(c.table.replicas, have.region) == nullptr) {
                ++removed;
            }
        }
        const bool busy =
            std::any_of(d->replicas.begin(), d->replicas.end(), [](const table_replica& r) {
                return r.status == replica_status::creating || r.status == replica_status::deleting;
            });
        if (added.size() + removed > 1 || (busy && added.size() + removed == 1)) {
            return failure("LimitExceededException",
                           "Only one replica can be created or deleted at a time per table");
        }
        if (!added.empty() && (c.table.region.empty() || added.front().region == c.table.region)) {
            return invalid("A replica must name another region than the table's");
        }
        std::erase_if(d->replicas, [&](const table_replica& r) {
            return r.status == replica_status::deleting &&
                   named(c.table.replicas, r.region) == nullptr;
        });
        for (auto& have : d->replicas) {
            const auto* want = named(c.table.replicas, have.region);
            if (want == nullptr) {
                have.status = replica_status::deleting;
                continue;
            }
            if (have.status == replica_status::creating &&
                (want->status == replica_status::active ||
                 want->status == replica_status::creation_failed)) {
                have.status = want->status;
                have.failure = want->failure;
            }
            have.backfilled = have.backfilled || want->backfilled;
        }
        if (!added.empty()) {
            auto r = added.front();
            // A peer's notice of a replica another cluster created adds
            // it ACTIVE; one asked for here starts CREATING.
            if (r.status != replica_status::active) {
                r.status = replica_status::creating;
            }
            r.created_ms = c.now_ms;
            r.backfilled = false;
            // A peer told of a new replica starts it where it is with the
            // replica that created it, so nothing in flight is missed.
            r.failure.clear();
            d->replicas.push_back(std::move(r));
            if (d->region.empty()) {
                d->region = c.table.region;
            }
        }
        if (d->replicas.empty()) {
            d->region.clear();
        }
        ++d->revision;
        store(batch, *d);
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    /// `replica_progress` (task 22): see the command.
    auto replica_progress(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        auto d = describe(c.table.name);
        if (!d || d->status == table_status::deleting) {
            return not_found(c.table.name);
        }
        for (const auto& got : c.table.replicas) {
            const auto it =
                std::find_if(d->replicas.begin(), d->replicas.end(),
                             [&](const table_replica& r) { return r.region == got.region; });
            if (it == d->replicas.end()) {
                continue;
            }
            std::vector<std::pair<std::string, std::uint64_t>> merged;
            merged.reserve(got.cursors.size());
            for (const auto& [key, seq] : got.cursors) {
                auto next = seq;
                for (const auto& [old_key, old_seq] : it->cursors) {
                    if (old_key == key) {
                        next = std::max(next, old_seq);
                    }
                }
                merged.emplace_back(key, next);
            }
            it->cursors = std::move(merged);
        }
        ++d->revision;
        store(batch, *d);
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    static auto invalid(std::string message) -> catalog_result {
        catalog_result r;
        r.error_type = dynamodb_error_style().validation_type;
        r.error_message = std::move(message);
        return r;
    }

    auto trim_ring() -> void {
        const auto durable = engine().durable_sequence();
        while (_ring.size() > 1 && _ring[1].second <= durable) {
            _durable_floor = _ring.front().first;
            _ring.pop_front();
        }
    }

    engine_type* _engine = nullptr;
    LogIndex _applied{};
    LogIndex _durable_floor{};
    std::deque<std::pair<LogIndex, kythira::engine_sequence>> _ring;
};

static_assert(kythira::persistent_state_machine<dynamo_catalog_state_machine<>>);

/// The `drop_table_step` entry the catalog leader proposes to an item group
/// for a delete job, deleting at most `max_keys` items.
[[nodiscard]] inline auto drop_step_for(const delete_job& job, std::uint32_t max_keys)
    -> item_command {
    item_command c;
    c.op = item_command::kind::drop_table_step;
    c.table_id = job.table_id;
    c.schema = job.schema;
    c.max_keys = max_keys;
    return c;
}

}  // namespace kythira::dynamodb
