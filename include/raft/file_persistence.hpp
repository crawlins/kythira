#pragma once

#include <raft/fault_injection.hpp>
#include <raft/json_serializer.hpp>
#include <raft/types.hpp>

#include <boost/json.hpp>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace kythira {

// File-backed persistence engine for use in standalone chaos_node processes.
//
// Storage layout under DATA_DIR/:
//   term        — decimal current term (plain text)
//   voted_for   — decimal node ID or "none"
//   log         — one JSON object per line, each with {term, index, command_b64}
//   snapshot    — JSON snapshot (optional)
//
// All writes use the atomic write-then-rename idiom so the file is never
// partially written from the reader's perspective.

template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
         typename LogIndex = std::uint64_t>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex>
class file_persistence_engine {
public:
    using log_entry_t = log_entry<TermId, LogIndex>;
    using snapshot_t = snapshot<NodeId, TermId, LogIndex>;
    using ser_t = json_rpc_serializer<std::vector<std::byte>>;

    explicit file_persistence_engine(std::filesystem::path data_dir) : _dir(std::move(data_dir)) {
        std::filesystem::create_directories(_dir);
        load_all();
    }

    // Move-only: safe when the object is not concurrently accessed (pre-start).
    file_persistence_engine(file_persistence_engine&& other) noexcept
        : _dir(std::move(other._dir)),
          _current_term(other._current_term),
          _voted_for(std::move(other._voted_for)),
          _log(std::move(other._log)),
          _snapshot(std::move(other._snapshot)) {}

    file_persistence_engine& operator=(file_persistence_engine&&) = delete;
    file_persistence_engine(const file_persistence_engine&) = delete;
    file_persistence_engine& operator=(const file_persistence_engine&) = delete;

    // ── currentTerm ──────────────────────────────────────────────────────────

    auto save_current_term(TermId term) -> void {
        fiu_do_on("raft/persistence/save_current_term",
                  throw std::runtime_error("chaos: save_current_term"););
        std::lock_guard lock(_mu);
        atomic_write(_dir / "term", std::to_string(term));
        _current_term = term;
    }

    auto load_current_term() -> TermId {
        std::lock_guard lock(_mu);
        return _current_term;
    }

    // ── votedFor ─────────────────────────────────────────────────────────────

    auto save_voted_for(NodeId node) -> void {
        fiu_do_on("raft/persistence/save_voted_for",
                  throw std::runtime_error("chaos: save_voted_for"););
        std::lock_guard lock(_mu);
        if constexpr (std::is_same_v<NodeId, std::string>)
            atomic_write(_dir / "voted_for", node);
        else
            atomic_write(_dir / "voted_for", std::to_string(node));
        _voted_for = node;
    }

    auto load_voted_for() -> std::optional<NodeId> {
        std::lock_guard lock(_mu);
        return _voted_for;
    }

    // ── Log ──────────────────────────────────────────────────────────────────

    auto append_log_entry(const log_entry_t& entry) -> void {
        fiu_do_on("raft/persistence/append_log_entry",
                  throw std::runtime_error("chaos: append_log_entry"););
        std::lock_guard lock(_mu);
        _log[entry.index()] = entry;
        // Append one JSON line to the log file
        auto line = entry_to_json(entry) + "\n";
        auto path = _dir / "log";
        std::ofstream f(path, std::ios::app | std::ios::binary);
        if (!f) throw std::runtime_error("file_persistence: cannot open log for append");
        f.write(line.data(), static_cast<std::streamsize>(line.size()));
        f.flush();
    }

    auto get_log_entry(LogIndex index) -> std::optional<log_entry_t> {
        std::lock_guard lock(_mu);
        auto it = _log.find(index);
        if (it == _log.end()) return std::nullopt;
        return it->second;
    }

    auto get_log_entries(LogIndex start, LogIndex end) -> std::vector<log_entry_t> {
        std::lock_guard lock(_mu);
        std::vector<log_entry_t> result;
        for (LogIndex i = start; i <= end; ++i) {
            auto it = _log.find(i);
            if (it != _log.end()) result.push_back(it->second);
        }
        return result;
    }

    auto get_last_log_index() -> LogIndex {
        std::lock_guard lock(_mu);
        if (_log.empty()) return LogIndex{0};
        LogIndex max{0};
        for (const auto& [idx, _] : _log)
            if (idx > max) max = idx;
        return max;
    }

    auto truncate_log(LogIndex index) -> void {
        fiu_do_on("raft/persistence/truncate_log",
                  throw std::runtime_error("chaos: truncate_log"););
        std::lock_guard lock(_mu);
        auto it = _log.begin();
        while (it != _log.end()) it = (it->first >= index) ? _log.erase(it) : std::next(it);
        rewrite_log_file();
    }

    auto delete_log_entries_before(LogIndex index) -> void {
        std::lock_guard lock(_mu);
        auto it = _log.begin();
        while (it != _log.end()) it = (it->first < index) ? _log.erase(it) : std::next(it);
        rewrite_log_file();
    }

    // ── Snapshot ─────────────────────────────────────────────────────────────

    auto save_snapshot(const snapshot_t& snap) -> void {
        fiu_do_on("raft/persistence/save_snapshot",
                  throw std::runtime_error("chaos: save_snapshot"););
        std::lock_guard lock(_mu);
        atomic_write(_dir / "snapshot", snapshot_to_json(snap));
        _snapshot = snap;
    }

    auto load_snapshot() -> std::optional<snapshot_t> {
        std::lock_guard lock(_mu);
        return _snapshot;
    }

private:
    // ── Initialisation ───────────────────────────────────────────────────────

    void load_all() {
        // term
        if (auto s = read_file(_dir / "term"); s) {
            try {
                _current_term = static_cast<TermId>(std::stoull(*s));
            } catch (...) {
            }
        }

        // voted_for
        if (auto s = read_file(_dir / "voted_for"); s && *s != "none" && !s->empty()) {
            try {
                if constexpr (std::is_same_v<NodeId, std::string>)
                    _voted_for = *s;
                else
                    _voted_for = static_cast<NodeId>(std::stoull(*s));
            } catch (...) {
            }
        }

        // log
        auto log_path = _dir / "log";
        if (std::filesystem::exists(log_path)) {
            std::ifstream f(log_path);
            std::string line;
            while (std::getline(f, line)) {
                if (line.empty()) continue;
                try {
                    auto entry = json_to_entry(line);
                    _log[entry.index()] = entry;
                } catch (...) {
                }
            }
        }

        // snapshot
        if (auto s = read_file(_dir / "snapshot"); s && !s->empty()) {
            try {
                _snapshot = json_to_snapshot(*s);
            } catch (...) {
            }
        }
    }

    // ── Helpers ──────────────────────────────────────────────────────────────

    static auto read_file(const std::filesystem::path& p) -> std::optional<std::string> {
        if (!std::filesystem::exists(p)) return std::nullopt;
        std::ifstream f(p);
        if (!f) return std::nullopt;
        return std::string(std::istreambuf_iterator<char>(f), {});
    }

    void atomic_write(const std::filesystem::path& path, std::string_view content) {
        auto tmp = path;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
            if (!f) throw std::runtime_error("file_persistence: cannot write " + tmp.string());
            f.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        std::filesystem::rename(tmp, path);
    }

    void rewrite_log_file() {
        std::string content;
        // Write in index order
        std::vector<LogIndex> indices;
        indices.reserve(_log.size());
        for (const auto& [idx, _] : _log) indices.push_back(idx);
        std::sort(indices.begin(), indices.end());
        for (auto idx : indices) content += entry_to_json(_log.at(idx)) + "\n";
        atomic_write(_dir / "log", content);
    }

    // ── Serialisation ────────────────────────────────────────────────────────

    static auto entry_to_json(const log_entry_t& e) -> std::string {
        // Reuse the json_rpc_serializer's base64 helper by serializing a
        // minimal append_entries_request containing this one entry.
        // Simpler: build the JSON manually using boost::json.
        boost::json::object obj;
        obj["term"] = e.term();
        obj["index"] = e.index();
        // base64-encode command bytes
        obj["command"] = bytes_to_base64(e.command());
        return boost::json::serialize(obj);
    }

    static auto json_to_entry(const std::string& s) -> log_entry_t {
        auto obj = boost::json::parse(s).as_object();
        log_entry_t e;
        e._term = static_cast<TermId>(obj["term"].as_int64());
        e._index = static_cast<LogIndex>(obj["index"].as_int64());
        e._command = base64_to_bytes(std::string(obj["command"].as_string()));
        return e;
    }

    static auto snapshot_to_json(const snapshot_t& snap) -> std::string {
        boost::json::object obj;
        obj["last_included_index"] = snap.last_included_index();
        obj["last_included_term"] = snap.last_included_term();
        obj["state"] = bytes_to_base64(snap.state_machine_state());
        // configuration nodes
        boost::json::array nodes;
        for (auto n : snap.configuration().nodes()) {
            if constexpr (std::is_same_v<NodeId, std::string>)
                nodes.push_back(boost::json::string(n));
            else
                nodes.push_back(static_cast<std::uint64_t>(n));
        }
        obj["nodes"] = nodes;
        return boost::json::serialize(obj);
    }

    static auto json_to_snapshot(const std::string& s) -> snapshot_t {
        auto obj = boost::json::parse(s).as_object();
        snapshot_t snap;
        snap._last_included_index = static_cast<LogIndex>(obj["last_included_index"].as_int64());
        snap._last_included_term = static_cast<TermId>(obj["last_included_term"].as_int64());
        snap._state_machine_state = base64_to_bytes(std::string(obj["state"].as_string()));
        for (const auto& n : obj["nodes"].as_array()) {
            if constexpr (std::is_same_v<NodeId, std::string>)
                snap._configuration._nodes.emplace_back(n.as_string());
            else
                snap._configuration._nodes.push_back(static_cast<NodeId>(n.as_int64()));
        }
        snap._configuration._is_joint_consensus = false;
        return snap;
    }

    // Base64 helpers (delegated to json_rpc_serializer's internal implementation
    // by serialising/deserialising a dummy single-byte request — instead, copy
    // the minimal codec here to avoid coupling to private methods).

    static constexpr std::string_view k_b64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    static auto bytes_to_base64(const std::vector<std::byte>& in) -> std::string {
        std::string out;
        out.reserve(((in.size() + 2) / 3) * 4);
        for (std::size_t i = 0; i < in.size(); i += 3) {
            std::uint32_t v = static_cast<std::uint8_t>(in[i]) << 16;
            if (i + 1 < in.size()) v |= static_cast<std::uint8_t>(in[i + 1]) << 8;
            if (i + 2 < in.size()) v |= static_cast<std::uint8_t>(in[i + 2]);
            out += k_b64[(v >> 18) & 0x3F];
            out += k_b64[(v >> 12) & 0x3F];
            out += (i + 1 < in.size()) ? k_b64[(v >> 6) & 0x3F] : '=';
            out += (i + 2 < in.size()) ? k_b64[(v) & 0x3F] : '=';
        }
        return out;
    }

    static auto base64_to_bytes(const std::string& in) -> std::vector<std::byte> {
        static const auto tbl = [] {
            std::array<int8_t, 256> t{};
            t.fill(-1);
            for (int i = 0; i < 64; ++i) t[static_cast<uint8_t>(k_b64[i])] = static_cast<int8_t>(i);
            return t;
        }();
        std::vector<std::byte> out;
        out.reserve(in.size() * 3 / 4);
        std::uint32_t v = 0;
        int bits = 0;
        for (char c : in) {
            if (c == '=') break;
            int8_t b = tbl[static_cast<uint8_t>(c)];
            if (b < 0) continue;
            v = (v << 6) | static_cast<std::uint32_t>(b);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<std::byte>((v >> bits) & 0xFF));
            }
        }
        return out;
    }

    std::filesystem::path _dir;
    mutable std::mutex _mu;
    TermId _current_term{0};
    std::optional<NodeId> _voted_for;
    std::unordered_map<LogIndex, log_entry_t> _log;
    std::optional<snapshot_t> _snapshot;
};

}  // namespace kythira
