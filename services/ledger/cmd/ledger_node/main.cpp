// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// ledger_node: the verifiable ledger daemon (.kiro/specs/verifiable-ledger/
// task 7c, doc/verifiable_ledger.md). Configuration is read from the
// environment (config.cpp, `ledger_node --help`); run.cpp holds the stack.

#if defined(KYTHIRA_LEDGER_NODE_DAEMON)
#include "config.hpp"
#endif

#include <kythira/ledger/ion_hash.hpp>
#include <kythira/ledger/merkle.hpp>

#if defined(KYTHIRA_LEDGER_NODE_DAEMON) && !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && \
    !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <cstdio>
#include <exception>
#include <string_view>

namespace {

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
#if defined(KYTHIRA_LEDGER_NODE_DAEMON)
    if (argc == 2 && (arg == "--help" || arg == "-h")) {
        std::fputs(kythira::ledger_node::usage_text().data(), stdout);
        return 0;
    }
    if (argc > 1) {
        std::fprintf(stderr, "ledger_node: unknown argument %s\n\n%s", argv[1],
                     kythira::ledger_node::usage_text().data());
        return 2;
    }
#else
    // Built without RocksDB or the TLS stack (CONFIG_ENGINE_ROCKSDB and a
    // future backend with OpenSSL): only the self-test.
    std::fputs(
        "usage: ledger_node --self-test\n\nThis ledger_node was built without the daemon "
        "(it needs CONFIG_ENGINE_ROCKSDB, OpenSSL and a future backend).\n",
        argc == 2 && (arg == "--help" || arg == "-h") ? stdout : stderr);
    return argc == 2 && (arg == "--help" || arg == "-h") ? 0 : 2;
#endif
#if defined(KYTHIRA_LEDGER_NODE_DAEMON)
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
    folly::Init init(&argc, &argv);
#endif
    kythira::ledger_node::node_options opt;
    try {
        opt = kythira::ledger_node::from_env();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 2;
    }
    kythira::ledger_node::install_signal_handlers();
    try {
        return kythira::ledger_node::run(opt);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ledger_node: %s\n", e.what());
        return 1;
    }
#endif
}
