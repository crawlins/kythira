// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// kinesis_gateway_node: the Kinesis Data Streams-compatible daemon
// (.kiro/specs/kinesis-compatible-streams/).
//
// A placeholder until task 6, which brings the stream lifecycle, writes and
// the multi-raft host; the front end arrives in task 4. Today it only links
// the module, so the target, its build flags and the module boundary are
// exercised from task 1 on, and it reports what it is when asked.

#include <kythira/kinesis/kinesis_operations.hpp>

#include <algorithm>
#include <cstdio>
#include <string_view>

namespace {

constexpr std::string_view usage =
    "usage: kinesis_gateway_node [--help | --self-test]\n"
    "\n"
    "The Kythira Kinesis Data Streams-compatible daemon. Not implemented yet:\n"
    "the server arrives with .kiro/specs/kinesis-compatible-streams/ tasks 4-6.\n"
    "\n"
    "  --help       print this text\n"
    "  --self-test  check the generated operation table, then exit\n";

auto self_test() -> int {
    namespace k = kythira::kinesis;
    const auto& ops = k::operations;
    const bool sorted = std::is_sorted(
        ops.begin(), ops.end(), [](const auto& a, const auto& b) { return a._name < b._name; });
    if (!sorted) {
        std::fputs("self-test: operation table is not sorted\n", stderr);
        return 1;
    }
    const auto* subscribe = std::find_if(
        ops.begin(), ops.end(), [](const auto& o) { return o._name == "SubscribeToShard"; });
    if (subscribe == ops.end() || !subscribe->_event_stream) {
        std::fputs("self-test: SubscribeToShard missing or not an event stream\n", stderr);
        return 1;
    }
    const auto streams =
        std::count_if(ops.begin(), ops.end(), [](const auto& o) { return o._event_stream; });
    std::printf(
        "self-test ok: %zu operations (%td event stream), target prefix %.*s, botocore %.*s\n",
        ops.size(), streams, static_cast<int>(k::target_prefix.size()), k::target_prefix.data(),
        static_cast<int>(k::botocore_version.size()), k::botocore_version.data());
    return 0;
}

}  // namespace

auto main(int argc, char** argv) -> int {
    const std::string_view arg = argc > 1 ? std::string_view(argv[1]) : std::string_view();
    if (argc == 2 && arg == "--self-test") {
        return self_test();
    }
    std::fputs(usage.data(), argc == 2 && arg == "--help" ? stdout : stderr);
    return argc == 2 && arg == "--help" ? 0 : 2;
}
