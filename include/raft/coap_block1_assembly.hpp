// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Server-side Block1 reassembly (RFC 7959 Section 2.5), shared by the CoAP
// backends wherever the library underneath does not do it: inner Block1 on
// the libcoap OSCORE path, and outer Block1 on libnyoci and on cantcoap's
// OSCORE path. Library-neutral: the caller decodes the option and answers
// with whatever its own library spells 2.31, 4.08, 4.13 and 5.03.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira {

enum class block1_outcome {
    more,        ///< stored; answer 2.31 Continue echoing the block
    complete,    ///< last block in; `body` holds the whole request
    incomplete,  ///< out of order or a block missing; answer 4.08
    too_large,   ///< past the body limit; answer 4.13
    busy,        ///< too many uploads open; answer 5.03
};

struct block1_result {
    block1_outcome outcome{block1_outcome::more};
    std::vector<std::byte> body;
};

/// Partial request bodies keyed by whatever identifies one upload: the peer
/// and the resource, so two peers (or two RPC kinds from one peer) uploading
/// at once do not interleave. Not thread-safe; each backend calls it from
/// its single I/O thread.
class block1_assembly {
public:
    /// Take one block. `max_body` bounds a reassembled body, `max_uploads`
    /// the number open at once, so an authenticated peer cannot grow the
    /// map without bound by opening uploads it never finishes.
    auto offer(const std::string& key, std::uint32_t number, bool more, std::size_t block_size,
               std::span<const std::byte> chunk, std::size_t max_body, std::size_t max_uploads)
        -> block1_result {
        auto found = _uploads.find(key);
        if (number == 0 && found != _uploads.end()) {
            found->second.clear();  // a restarted upload
        }
        if (found == _uploads.end()) {
            if (_uploads.size() >= max_uploads) {
                return {block1_outcome::busy, {}};
            }
            found = _uploads.emplace(key, std::vector<std::byte>{}).first;
        }
        auto& body = found->second;
        if (static_cast<std::size_t>(number) * block_size != body.size()) {
            _uploads.erase(found);
            return {block1_outcome::incomplete, {}};
        }
        if (body.size() + chunk.size() > max_body) {
            _uploads.erase(found);
            return {block1_outcome::too_large, {}};
        }
        body.insert(body.end(), chunk.begin(), chunk.end());
        if (more) {
            return {block1_outcome::more, {}};
        }
        block1_result done{block1_outcome::complete, std::move(body)};
        _uploads.erase(found);
        return done;
    }

    auto clear() -> void { _uploads.clear(); }
    [[nodiscard]] auto open_uploads() const -> std::size_t { return _uploads.size(); }

private:
    std::unordered_map<std::string, std::vector<std::byte>> _uploads;
};

}  // namespace kythira
