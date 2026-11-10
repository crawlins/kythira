// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file kinesis_records.hpp
/// @brief Reading one shard's records from the segment store
///        (`.kiro/specs/kinesis-compatible-streams/` task 7, Requirement
///        9.3, 9.4): what `GetRecords` serves, on any replica.
///
/// The reader takes a copy of the shard's replicated state (its sealed
/// segment index, last counter and trim point) and walks the index segment
/// by segment, so a sealed segment this replica lacks is fetched from a
/// peer through the state machine's fetcher, never silently skipped; if it
/// cannot be fetched the read reports it missing and `GetRecords` fails
/// with InternalFailure (design, "a segment failing its SHA-256 on fetch").
/// Past the sealed index it reads the active segment.
///
/// Positions below the trim point start at the trim point, as AWS starts a
/// trimmed iterator at the trim horizon (9.4). A minimum timestamp
/// (`AT_TIMESTAMP`) skips whole sealed segments by their last timestamp and
/// then single records; arrival times never decrease within a shard, so the
/// first record at or after it starts the answer.

#include <kythira/kinesis/kinesis_stream_state_machine.hpp>

#include <raft/record_segment_store.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace kythira::kinesis {

struct read_record {
    std::uint64_t _counter = 0;
    std::int64_t _arrival_ms = 0;
    stored_record _record;
};

struct shard_read {
    std::vector<read_record> _records;
    /// Where the next read starts: one past the last record looked at.
    std::uint64_t _next_counter = 0;
    /// Nothing past `_next_counter` exists yet.
    bool _caught_up = false;
    /// A sealed segment in the range is neither here nor fetchable.
    bool _missing = false;
};

/// Records of shard `key` at or after counter `from` (and, when
/// `min_timestamp_ms` is positive, arrival time), within `limits`. `state`
/// is the shard's state as copied from the replica. Throws
/// `record_segment_error` if a record fails its CRC.
[[nodiscard]] inline auto read_shard(const record_segment_store& store,
                                     const segment_fetcher& fetch, const std::string& key,
                                     const shard_state& state, std::uint64_t from,
                                     std::int64_t min_timestamp_ms,
                                     const segment_read_limits& limits) -> shard_read {
    shard_read out;
    out._next_counter = std::max({from, state._trim_before, std::uint64_t{1}});
    std::size_t records_left = limits._max_records;
    std::uint64_t bytes_left = limits._max_bytes;
    const auto full = [&] { return records_left == 0 || bytes_left == 0; };
    const auto take = [&](const segment_read_result& r) {
        for (const auto& rec : r._records) {
            out._next_counter = rec._seq + 1;
            records_left = records_left > 0 ? records_left - 1 : 0;
            bytes_left -= std::min<std::uint64_t>(bytes_left, rec._data.size());
            if (rec._timestamp_ms < min_timestamp_ms) {
                continue;
            }
            out._records.push_back({._counter = rec._seq,
                                    ._arrival_ms = rec._timestamp_ms,
                                    ._record = decode_stored_record(rec._data)});
        }
        if (r._records.empty()) {
            out._next_counter = std::max(out._next_counter, r._next_seq);
        }
    };
    const auto remaining = [&] {
        return segment_read_limits{._max_records = records_left, ._max_bytes = bytes_left};
    };

    for (const auto& d : state._sealed) {
        if (full()) {
            break;
        }
        if (d._last_seq < out._next_counter) {
            continue;
        }
        if (min_timestamp_ms > 0 && d._last_timestamp_ms < min_timestamp_ms) {
            out._next_counter = d._last_seq + 1;
            continue;
        }
        auto r = store.read_segment(key, d._first_seq, out._next_counter, remaining());
        if (!r && fetch) {
            try {
                fetch(d);
            } catch (const std::exception&) {
                // Reported as missing below.
            }
            r = store.read_segment(key, d._first_seq, out._next_counter, remaining());
        }
        if (!r) {
            out._missing = true;
            return out;
        }
        take(*r);
        if (!r->_segment_exhausted && !full()) {
            // A sealed segment that stops short of its descriptor: never
            // skip past what it should hold.
            out._missing = true;
            return out;
        }
    }
    if (!full()) {
        take(store.read(key, out._next_counter, remaining()));
    }
    const auto tip = std::max(state._last_counter, store.last_seq(key).value_or(0));
    out._caught_up = out._next_counter > tip;
    return out;
}

}  // namespace kythira::kinesis
