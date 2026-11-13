// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The aws-sdk-cpp producer and polling consumer of the Kinesis container
// scenario (.kiro/specs/kinesis-compatible-streams/ task 7, Requirement
// 9.4): the same three commands as kinesis_e2e.py's boto3 client, through
// the C++ SDK's KinesisClient, so the scenario checks a third SDK's
// signing, JSON shapes and pagination against the cluster.
//
//     kinesis_sdk_cpp create  --endpoint URL --stream S --shards N
//     kinesis_sdk_cpp produce --endpoint URL --stream S --count N --tag T
//     kinesis_sdk_cpp consume --endpoints URL,URL --stream S --expect N
//                             [--timeout SECONDS] [--limit N]
//
// `produce` writes `T-0` .. `T-<N-1>` with PutRecords in batches of 100,
// retrying the entries that fail. `consume` reads every shard from
// TRIM_HORIZON, each GetRecords from the next endpoint in turn, and fails
// unless sequence numbers rise within each shard and every record arrives
// exactly once. Each prints one JSON line; a failure exits non-zero with
// the reason on stderr. Credentials come from AWS_ACCESS_KEY_ID and
// AWS_SECRET_ACCESS_KEY.

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/kinesis/KinesisClient.h>
#include <aws/kinesis/model/CreateStreamRequest.h>
#include <aws/kinesis/model/DescribeStreamSummaryRequest.h>
#include <aws/kinesis/model/GetRecordsRequest.h>
#include <aws/kinesis/model/GetShardIteratorRequest.h>
#include <aws/kinesis/model/ListShardsRequest.h>
#include <aws/kinesis/model/PutRecordsRequest.h>
#include <aws/kinesis/model/PutRecordsRequestEntry.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace km = Aws::Kinesis::Model;
using clock_type = std::chrono::steady_clock;

[[noreturn]] void fail(const std::string& why) {
    std::cerr << why << '\n';
    std::exit(1);
}

/// `--name value` pairs after the command.
class options {
public:
    options(int argc, char** argv) {
        for (int i = 2; i + 1 < argc; i += 2) {
            const std::string name(argv[i]);
            if (!name.starts_with("--")) {
                fail("expected an option, got " + name);
            }
            _values[name.substr(2)] = argv[i + 1];
        }
    }

    [[nodiscard]] auto text(const std::string& name) const -> std::string {
        const auto it = _values.find(name);
        if (it == _values.end()) {
            fail("--" + name + " is required");
        }
        return it->second;
    }

    [[nodiscard]] auto number(const std::string& name, std::optional<long> fallback = {}) const
        -> long {
        const auto it = _values.find(name);
        if (it == _values.end()) {
            if (!fallback) {
                fail("--" + name + " is required");
            }
            return *fallback;
        }
        return std::stol(it->second);
    }

private:
    std::map<std::string, std::string> _values;
};

auto client(const std::string& endpoint) -> std::unique_ptr<Aws::Kinesis::KinesisClient> {
    Aws::Client::ClientConfiguration cfg;
    cfg.region = "us-east-1";
    cfg.endpointOverride = endpoint;
    cfg.scheme =
        endpoint.starts_with("https://") ? Aws::Http::Scheme::HTTPS : Aws::Http::Scheme::HTTP;
    cfg.connectTimeoutMs = 5000;
    cfg.requestTimeoutMs = 20000;
    // The plain polling path; SubscribeToShard is the boto3 client's case.
    cfg.retryStrategy = Aws::Client::InitRetryStrategy("standard");
    return std::make_unique<Aws::Kinesis::KinesisClient>(
        std::make_shared<Aws::Auth::EnvironmentAWSCredentialsProvider>(), nullptr, cfg);
}

auto split(const std::string& list) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::stringstream in(list);
    std::string item;
    while (std::getline(in, item, ',')) {
        if (!item.empty()) {
            out.push_back(item);
        }
    }
    return out;
}

template<typename Outcome> auto checked(Outcome&& outcome, const std::string& what) {
    if (!outcome.IsSuccess()) {
        fail(what + " failed: " + outcome.GetError().GetExceptionName() + ": " +
             outcome.GetError().GetMessage());
    }
    return std::forward<Outcome>(outcome).GetResultWithOwnership();
}

auto create(const options& o) -> void {
    auto c = client(o.text("endpoint"));
    const auto stream = o.text("stream");
    const auto shards = o.number("shards");
    km::CreateStreamRequest req;
    req.SetStreamName(stream);
    req.SetShardCount(static_cast<int>(shards));
    (void)checked(c->CreateStream(req), "CreateStream");
    const auto deadline = clock_type::now() + std::chrono::seconds(60);
    while (clock_type::now() < deadline) {
        km::DescribeStreamSummaryRequest d;
        d.SetStreamName(stream);
        const auto s = checked(c->DescribeStreamSummary(d), "DescribeStreamSummary");
        if (s.GetStreamDescriptionSummary().GetStreamStatus() == km::StreamStatus::ACTIVE) {
            std::cout << R"({"stream": ")" << stream << R"(", "shards": )" << shards << "}\n";
            return;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    fail(stream + " never became ACTIVE");
}

auto produce(const options& o) -> void {
    auto c = client(o.text("endpoint"));
    const auto stream = o.text("stream");
    const auto tag = o.text("tag");
    const auto count = o.number("count");
    std::vector<std::pair<std::string, std::string>> pending;
    for (long i = 0; i < count; ++i) {
        pending.emplace_back(tag + "-" + std::to_string(i), "pk-" + tag + "-" + std::to_string(i));
    }
    std::map<std::string, long> per_shard;
    const auto deadline = clock_type::now() + std::chrono::seconds(120);
    while (!pending.empty()) {
        if (clock_type::now() > deadline) {
            fail(std::to_string(pending.size()) + " records still failing after 120 s");
        }
        const auto n = std::min<std::size_t>(100, pending.size());
        const std::vector batch(pending.begin(), pending.begin() + static_cast<long>(n));
        pending.erase(pending.begin(), pending.begin() + static_cast<long>(n));
        km::PutRecordsRequest req;
        req.SetStreamName(stream);
        for (const auto& [data, key] : batch) {
            km::PutRecordsRequestEntry e;
            e.SetData(Aws::Utils::ByteBuffer(reinterpret_cast<const unsigned char*>(data.data()),
                                             data.size()));
            e.SetPartitionKey(key);
            req.AddRecords(std::move(e));
        }
        const auto out = checked(c->PutRecords(req), "PutRecords");
        const auto& results = out.GetRecords();
        if (results.size() != batch.size()) {
            fail("PutRecords answered " + std::to_string(results.size()) + " entries for " +
                 std::to_string(batch.size()));
        }
        std::vector<std::pair<std::string, std::string>> retry;
        for (std::size_t i = 0; i < batch.size(); ++i) {
            if (!results[i].GetErrorCode().empty()) {
                retry.push_back(batch[i]);
            } else {
                ++per_shard[results[i].GetShardId()];
            }
        }
        if (!retry.empty()) {
            // The keys are unique, so a retried record may go last.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            pending.insert(pending.begin(), retry.begin(), retry.end());
        }
    }
    std::cout << R"({"produced": )" << count << R"(, "per_shard": {)";
    const char* sep = "";
    for (const auto& [shard, n] : per_shard) {
        std::cout << sep << '"' << shard << R"(": )" << n;
        sep = ", ";
    }
    std::cout << "}}\n";
}

auto consume(const options& o) -> void {
    std::vector<std::unique_ptr<Aws::Kinesis::KinesisClient>> clients;
    for (const auto& e : split(o.text("endpoints"))) {
        clients.push_back(client(e));
    }
    if (clients.empty()) {
        fail("--endpoints names no endpoint");
    }
    const auto stream = o.text("stream");
    const auto expect = static_cast<std::size_t>(o.number("expect"));
    const auto limit = static_cast<int>(o.number("limit", 1000));
    auto& first = *clients.front();

    std::vector<std::string> shards;
    km::ListShardsRequest list;
    list.SetStreamName(stream);
    for (;;) {
        const auto page = checked(first.ListShards(list), "ListShards");
        for (const auto& s : page.GetShards()) {
            shards.push_back(s.GetShardId());
        }
        if (page.GetNextToken().empty()) {
            break;
        }
        list = km::ListShardsRequest();
        list.SetNextToken(page.GetNextToken());
    }
    std::map<std::string, std::string> iterators;
    for (const auto& sid : shards) {
        km::GetShardIteratorRequest req;
        req.SetStreamName(stream);
        req.SetShardId(sid);
        req.SetShardIteratorType(km::ShardIteratorType::TRIM_HORIZON);
        iterators[sid] =
            checked(first.GetShardIterator(req), "GetShardIterator").GetShardIterator();
    }

    std::set<std::string> seen;
    std::map<std::string, std::string> last_seq;
    std::map<std::string, long> per_shard;
    std::size_t turn = 0;
    const auto deadline = clock_type::now() + std::chrono::seconds(o.number("timeout", 60));
    // Sequence numbers are decimal strings of up to 56 digits: compare by
    // length, then text.
    const auto later = [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() > b.size() : a > b;
    };
    while (seen.size() < expect && clock_type::now() < deadline) {
        bool progressed = false;
        for (const auto& sid : shards) {
            auto& c = *clients[turn++ % clients.size()];
            km::GetRecordsRequest req;
            req.SetShardIterator(iterators[sid]);
            req.SetLimit(limit);
            const auto out = checked(c.GetRecords(req), "GetRecords on " + sid);
            for (const auto& r : out.GetRecords()) {
                const auto& seq = r.GetSequenceNumber();
                if (const auto it = last_seq.find(sid);
                    it != last_seq.end() && !later(seq, it->second)) {
                    fail(sid + ": sequence " + seq + " after " + it->second);
                }
                last_seq[sid] = seq;
                const auto& bytes = r.GetData();
                const std::string data(reinterpret_cast<const char*>(bytes.GetUnderlyingData()),
                                       bytes.GetLength());
                if (!seen.insert(data).second) {
                    fail(data + " arrived twice");
                }
                ++per_shard[sid];
                progressed = true;
            }
            if (out.GetNextShardIterator().empty()) {
                fail(sid + ": no NextShardIterator on an open shard");
            }
            iterators[sid] = out.GetNextShardIterator();
        }
        if (!progressed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    if (seen.size() != expect) {
        fail("read " + std::to_string(seen.size()) + " of " + std::to_string(expect) +
             " records before the deadline");
    }
    std::cout << R"({"consumed": )" << seen.size() << R"(, "per_shard": {)";
    const char* sep = "";
    for (const auto& sid : shards) {
        std::cout << sep << '"' << sid << R"(": )" << per_shard[sid];
        sep = ", ";
    }
    std::cout << "}}\n";
}

}  // namespace

auto main(int argc, char** argv) -> int {
    if (argc < 2) {
        fail("usage: kinesis_sdk_cpp create|produce|consume --option value ...");
    }
    const std::string command(argv[1]);
    const options o(argc, argv);
    Aws::SDKOptions sdk;
    Aws::InitAPI(sdk);
    int status = 0;
    try {
        if (command == "create") {
            create(o);
        } else if (command == "produce") {
            produce(o);
        } else if (command == "consume") {
            consume(o);
        } else {
            fail("unknown command " + command);
        }
    } catch (const std::exception& e) {
        std::cerr << command << ": " << e.what() << '\n';
        status = 1;
    }
    Aws::ShutdownAPI(sdk);
    return status;
}
