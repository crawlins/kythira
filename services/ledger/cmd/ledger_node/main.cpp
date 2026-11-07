// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// ledger_node: the verifiable ledger daemon (.kiro/specs/verifiable-ledger/).
//
// A placeholder until task 7, which brings the HTTP API, the ledger Raft
// groups and the data directory. Today it only links the module, so the
// target, its build flags and the module boundary are exercised from task 1
// on, and it reports what it is when asked.

#include <kythira/ledger/ion_hash.hpp>
#include <kythira/ledger/merkle.hpp>

#include <cstdio>
#include <string_view>

namespace {

constexpr std::string_view usage =
    "usage: ledger_node [--help | --self-test]\n"
    "\n"
    "The Kythira verifiable ledger daemon. Not implemented yet: the server\n"
    "arrives with .kiro/specs/verifiable-ledger/ task 7.\n"
    "\n"
    "  --help       print this text\n"
    "  --self-test  hash a fixed document and fold a two-leaf proof, then exit\n";

auto self_test() -> int {
    namespace ledger = kythira::ledger;
    const ledger::hash256 a = ledger::ion_hash("{ledger: \"kythira\", version: 1}");
    const ledger::hash256 b = ledger::ion_hash("{version: 1, ledger: \"kythira\"}");
    if (a != b) {
        std::fputs("self-test: Ion hash depends on field order\n", stderr);
        return 1;
    }
    ledger::memory_merkle_store store;
    ledger::merkle_accumulator acc(store);
    acc.append(a);
    acc.append(ledger::ion_hash("2"));
    if (!ledger::verify_proof(a, acc.proof(0, 2), acc.digest_at(2))) {
        std::fputs("self-test: proof did not verify\n", stderr);
        return 1;
    }
    std::printf("self-test ok: digest %s\n", ledger::to_hex(*acc.digest()).c_str());
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
