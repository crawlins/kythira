#pragma once

#include <raft/types.hpp>
#include <raft/exceptions.hpp>

#include <boost/json.hpp>
#include <vector>
#include <cstddef>
#include <string>

namespace raft {

// JSON RPC Serializer implementation
template<typename Data = std::vector<std::byte>>
requires std::ranges::range<Data> && std::same_as<std::ranges::range_value_t<Data>, std::byte>
class json_rpc_serializer {
public:
    // Serialize RequestVote Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
    auto serialize(const request_vote_request<NodeId, TermId, LogIndex>& req) const -> Data {
        boost::json::object obj;
        obj["type"] = "request_vote_request";
        obj["term"] = req.term();
        obj["candidate_id"] = req.candidate_id();
        obj["last_log_index"] = req.last_log_index();
        obj["last_log_term"] = req.last_log_term();
        
        return json_to_bytes(boost::json::serialize(obj));
    }
    
    // Serialize RequestVote Response
    template<typename TermId = std::uint64_t>
    auto serialize(const request_vote_response<TermId>& resp) const -> Data {
        boost::json::object obj;
        obj["type"] = "request_vote_response";
        obj["term"] = resp.term();
        obj["vote_granted"] = resp.vote_granted();
        
        return json_to_bytes(boost::json::serialize(obj));
    }
    
    // Serialize AppendEntries Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t, 
             typename LogIndex = std::uint64_t, typename LogEntry = log_entry<TermId, LogIndex>>
    auto serialize(const append_entries_request<NodeId, TermId, LogIndex, LogEntry>& req) const -> Data {
        boost::json::object obj;
        obj["type"] = "append_entries_request";
        obj["term"] = req.term();
        obj["leader_id"] = req.leader_id();
        obj["prev_log_index"] = req.prev_log_index();
        obj["prev_log_term"] = req.prev_log_term();
        obj["leader_commit"] = req.leader_commit();
        
        // Serialize entries
        boost::json::array entries_array;
        for (const auto& entry : req.entries()) {
            boost::json::object entry_obj;
            entry_obj["term"] = entry.term();
            entry_obj["index"] = entry.index();
            entry_obj["command"] = bytes_to_base64(entry.command());
            entries_array.push_back(entry_obj);
        }
        obj["entries"] = entries_array;
        
        return json_to_bytes(boost::json::serialize(obj));
    }
    
    // Serialize AppendEntries Response
    template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
    auto serialize(const append_entries_response<TermId, LogIndex>& resp) const -> Data {
        boost::json::object obj;
        obj["type"] = "append_entries_response";
        obj["term"] = resp.term();
        obj["success"] = resp.success();
        
        if (resp.conflict_index().has_value()) {
            obj["conflict_index"] = resp.conflict_index().value();
        }
        
        if (resp.conflict_term().has_value()) {
            obj["conflict_term"] = resp.conflict_term().value();
        }
        
        return json_to_bytes(boost::json::serialize(obj));
    }
    
    // Serialize InstallSnapshot Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
    auto serialize(const install_snapshot_request<NodeId, TermId, LogIndex>& req) const -> Data {
        boost::json::object obj;
        obj["type"] = "install_snapshot_request";
        obj["term"] = req.term();
        obj["leader_id"] = req.leader_id();
        obj["last_included_index"] = req.last_included_index();
        obj["last_included_term"] = req.last_included_term();
        obj["offset"] = req.offset();
        obj["data"] = bytes_to_base64(req.data());
        obj["done"] = req.done();
        
        return json_to_bytes(boost::json::serialize(obj));
    }
    
    // Serialize InstallSnapshot Response
    template<typename TermId = std::uint64_t>
    auto serialize(const install_snapshot_response<TermId>& resp) const -> Data {
        boost::json::object obj;
        obj["type"] = "install_snapshot_response";
        obj["term"] = resp.term();
        
        return json_to_bytes(boost::json::serialize(obj));
    }
    
    // Deserialize RequestVote Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
    auto deserialize_request_vote_request(const Data& data) const -> request_vote_request<NodeId, TermId, LogIndex> {
        auto json_str = bytes_to_string(data);
        auto obj = boost::json::parse(json_str).as_object();
        
        if (obj["type"].as_string() != "request_vote_request") {
            throw serialization_exception("Invalid message type for request_vote_request");
        }
        
        request_vote_request<NodeId, TermId, LogIndex> req;
        req._term = static_cast<TermId>(obj["term"].as_int64());
        req._last_log_index = static_cast<LogIndex>(obj["last_log_index"].as_int64());
        req._last_log_term = static_cast<TermId>(obj["last_log_term"].as_int64());
        
        if constexpr (std::same_as<NodeId, std::string>) {
            req._candidate_id = std::string(obj["candidate_id"].as_string());
        } else {
            req._candidate_id = static_cast<NodeId>(obj["candidate_id"].as_int64());
        }
        
        return req;
    }
    
    // Deserialize RequestVote Response
    template<typename TermId = std::uint64_t>
    auto deserialize_request_vote_response(const Data& data) const -> request_vote_response<TermId> {
        auto json_str = bytes_to_string(data);
        auto obj = boost::json::parse(json_str).as_object();
        
        if (obj["type"].as_string() != "request_vote_response") {
            throw serialization_exception("Invalid message type for request_vote_response");
        }
        
        request_vote_response<TermId> resp;
        resp._term = static_cast<TermId>(obj["term"].as_int64());
        resp._vote_granted = obj["vote_granted"].as_bool();
        
        return resp;
    }
    
    // Deserialize AppendEntries Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t, 
             typename LogIndex = std::uint64_t, typename LogEntry = log_entry<TermId, LogIndex>>
    auto deserialize_append_entries_request(const Data& data) const -> append_entries_request<NodeId, TermId, LogIndex, LogEntry> {
        auto json_str = bytes_to_string(data);
        auto obj = boost::json::parse(json_str).as_object();
        
        if (obj["type"].as_string() != "append_entries_request") {
            throw serialization_exception("Invalid message type for append_entries_request");
        }
        
        append_entries_request<NodeId, TermId, LogIndex, LogEntry> req;
        req._term = static_cast<TermId>(obj["term"].as_int64());
        req._prev_log_index = static_cast<LogIndex>(obj["prev_log_index"].as_int64());
        req._prev_log_term = static_cast<TermId>(obj["prev_log_term"].as_int64());
        req._leader_commit = static_cast<LogIndex>(obj["leader_commit"].as_int64());
        
        if constexpr (std::same_as<NodeId, std::string>) {
            req._leader_id = std::string(obj["leader_id"].as_string());
        } else {
            req._leader_id = static_cast<NodeId>(obj["leader_id"].as_int64());
        }
        
        // Deserialize entries
        const auto& entries_array = obj["entries"].as_array();
        for (const auto& entry_val : entries_array) {
            const auto& entry_obj = entry_val.as_object();
            LogEntry entry;
            entry._term = static_cast<TermId>(entry_obj.at("term").as_int64());
            entry._index = static_cast<LogIndex>(entry_obj.at("index").as_int64());
            entry._command = base64_to_bytes(std::string(entry_obj.at("command").as_string()));
            req._entries.push_back(entry);
        }
        
        return req;
    }
    
    // Deserialize AppendEntries Response
    template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
    auto deserialize_append_entries_response(const Data& data) const -> append_entries_response<TermId, LogIndex> {
        auto json_str = bytes_to_string(data);
        auto obj = boost::json::parse(json_str).as_object();
        
        if (obj["type"].as_string() != "append_entries_response") {
            throw serialization_exception("Invalid message type for append_entries_response");
        }
        
        append_entries_response<TermId, LogIndex> resp;
        resp._term = static_cast<TermId>(obj["term"].as_int64());
        resp._success = obj["success"].as_bool();
        
        if (obj.contains("conflict_index")) {
            resp._conflict_index = static_cast<LogIndex>(obj["conflict_index"].as_int64());
        }
        
        if (obj.contains("conflict_term")) {
            resp._conflict_term = static_cast<TermId>(obj["conflict_term"].as_int64());
        }
        
        return resp;
    }
    
    // Deserialize InstallSnapshot Request
    template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
    auto deserialize_install_snapshot_request(const Data& data) const -> install_snapshot_request<NodeId, TermId, LogIndex> {
        auto json_str = bytes_to_string(data);
        auto obj = boost::json::parse(json_str).as_object();
        
        if (obj["type"].as_string() != "install_snapshot_request") {
            throw serialization_exception("Invalid message type for install_snapshot_request");
        }
        
        install_snapshot_request<NodeId, TermId, LogIndex> req;
        req._term = static_cast<TermId>(obj["term"].as_int64());
        req._last_included_index = static_cast<LogIndex>(obj["last_included_index"].as_int64());
        req._last_included_term = static_cast<TermId>(obj["last_included_term"].as_int64());
        req._offset = static_cast<std::size_t>(obj["offset"].as_int64());
        req._data = base64_to_bytes(std::string(obj["data"].as_string()));
        req._done = obj["done"].as_bool();
        
        if constexpr (std::same_as<NodeId, std::string>) {
            req._leader_id = std::string(obj["leader_id"].as_string());
        } else {
            req._leader_id = static_cast<NodeId>(obj["leader_id"].as_int64());
        }
        
        return req;
    }
    
    // Deserialize InstallSnapshot Response
    template<typename TermId = std::uint64_t>
    auto deserialize_install_snapshot_response(const Data& data) const -> install_snapshot_response<TermId> {
        auto json_str = bytes_to_string(data);
        auto obj = boost::json::parse(json_str).as_object();
        
        if (obj["type"].as_string() != "install_snapshot_response") {
            throw serialization_exception("Invalid message type for install_snapshot_response");
        }
        
        install_snapshot_response<TermId> resp;
        resp._term = static_cast<TermId>(obj["term"].as_int64());
        
        return resp;
    }

private:
    // Helper to convert JSON string to bytes
    auto json_to_bytes(const std::string& json_str) const -> Data {
        Data result;
        if constexpr (requires { result.resize(0); }) {
            result.resize(json_str.size());
            std::transform(json_str.begin(), json_str.end(), result.begin(),
                          [](char c) { return static_cast<std::byte>(c); });
        }
        return result;
    }
    
    // Helper to convert bytes to string
    auto bytes_to_string(const Data& data) const -> std::string {
        std::string result;
        result.reserve(std::ranges::size(data));
        for (auto b : data) {
            result.push_back(static_cast<char>(b));
        }
        return result;
    }
    
    // Helper to convert bytes to base64
    auto bytes_to_base64(const std::vector<std::byte>& data) const -> std::string {
        static const char* base64_chars = 
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789+/";
        
        std::string result;
        int val = 0;
        int valb = -6;
        
        for (auto b : data) {
            val = (val << 8) + static_cast<unsigned char>(b);
            valb += 8;
            while (valb >= 0) {
                result.push_back(base64_chars[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        
        if (valb > -6) {
            result.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
        }
        
        while (result.size() % 4) {
            result.push_back('=');
        }
        
        return result;
    }
    
    // Helper to convert base64 to bytes
    auto base64_to_bytes(const std::string& base64) const -> std::vector<std::byte> {
        static const unsigned char base64_table[256] = {
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63,
            52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
            64,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
            15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64,
            64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
            41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64
        };
        
        std::vector<std::byte> result;
        int val = 0;
        int valb = -8;
        
        for (unsigned char c : base64) {
            if (base64_table[c] == 64) break;
            val = (val << 6) + base64_table[c];
            valb += 6;
            if (valb >= 0) {
                result.push_back(static_cast<std::byte>((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        
        return result;
    }
};

// Verify that json_rpc_serializer satisfies the rpc_serializer concept
static_assert(rpc_serializer<json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>,
              "json_rpc_serializer must satisfy the rpc_serializer concept");

} // namespace raft
