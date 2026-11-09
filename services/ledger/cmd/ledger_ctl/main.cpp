// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// ledger_ctl: check a ledger journal without trusting the cluster
// (.kiro/specs/verifiable-ledger/ task 10, Requirements 9.1-9.3).
//
//   ledger_ctl verify --export DIR [--digest FILE [--ca FILE]]
//   ledger_ctl verify --segments DIR [--digest FILE [--ca FILE]]
//   ledger_ctl prove --proof FILE --digest FILE [--ca FILE]
//
// With --ca, the digest file must be a CA-signed digest (Requirement 4.5)
// whose signature and certificate chain check against that CA bundle;
// that failing is a verification failure (1).
// Exit status: 0 verified, 1 verification failed (the first bad block is
// named), 2 usage or I/O error. Verifying through the API and from a live
// data directory needs ledger_node (task 7); an export downloaded from
// object storage, or a node's segment directory, can be checked now.

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_segments.hpp>
#include <kythira/ledger/journal_signing.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/journal_verifier.hpp>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ledger = kythira::ledger;

constexpr std::string_view usage =
    "usage: ledger_ctl verify (--export DIR | --segments DIR) [--digest FILE [--ca FILE]]\n"
    "       ledger_ctl prove --proof FILE --digest FILE [--ca FILE]\n"
    "\n"
    "verify   re-hash every block (revisions, entriesHash, blockHash), check\n"
    "         the chain, and check the digest at a saved digest's tip.\n"
    "         --export DIR    an ExportJournal directory (manifest.json and its\n"
    "                         objects; Ion text or Ion binary, not JSON)\n"
    "         --segments DIR  a node's segment directory (seg-*.kyl)\n"
    "prove    check a block or revision proof (as GetBlock or GetRevision\n"
    "         returns it, in Ion) against a saved digest.\n"
    "\n"
    "A digest file is Ion: {tip: <block seq>, digest: {{<base64>}}}, or a signed\n"
    "digest as GetDigest returns it from a node with a signing key. --ca FILE\n"
    "(a PEM CA bundle) requires the digest to be signed by a certificate from\n"
    "that CA, and checks the signature before anything else.\n"
    "Exit status: 0 verified, 1 verification failed, 2 usage or I/O error.\n";

struct usage_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

auto parse_flags(int argc, char** argv) -> std::map<std::string, std::string> {
    std::map<std::string, std::string> out;
    for (int i = 2; i < argc; i += 2) {
        const std::string flag = argv[i];
        if (!flag.starts_with("--") || i + 1 >= argc) {
            throw usage_error("expected --flag VALUE pairs, got " + flag);
        }
        out[flag.substr(2)] = argv[i + 1];
    }
    return out;
}

auto read_ion_file(const std::string& path) -> ledger::ion_value {
    const auto bytes = ledger::read_file(path);
    if (!bytes) {
        throw std::runtime_error("cannot read " + path);
    }
    return ledger::ion_value::parse(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes->data()), bytes->size()));
}

struct signature_failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Reads a digest file. With --ca the file must be a signed digest that
/// checks against that bundle (else signature_failure); without it a
/// signature is reported and left unchecked.
auto load_digest(const std::map<std::string, std::string>& flags, const std::string& path)
    -> ledger::saved_digest {
    const auto v = read_ion_file(path);
    const auto ca = flags.find("ca");
    if (!ledger::signed_digest::is_signed(v)) {
        if (ca != flags.end()) {
            throw signature_failure("the digest is not signed, and --ca requires a signature");
        }
        return ledger::saved_digest::from_ion(v);
    }
    const auto sd = ledger::signed_digest::from_ion(v);
    if (ca == flags.end()) {
        std::fprintf(stderr,
                     "ledger_ctl: note: the digest is signed; pass --ca to check the "
                     "signature\n");
        return sd.digest;
    }
    const auto bundle = ledger::read_file(ca->second);
    if (!bundle) {
        throw std::runtime_error("cannot read " + ca->second);
    }
    const auto check = ledger::check_signed_digest(sd, *bundle);
    if (check.failure) {
        throw signature_failure("digest signature: " + *check.failure);
    }
    std::printf("digest of ledger %s at block %llu signed by %s\n", sd.ledger.c_str(),
                static_cast<unsigned long long>(sd.digest.tip),
                check.signer.empty() ? "a certificate from the CA" : check.signer.front().c_str());
    return sd.digest;
}

/// The blocks of an export or segment directory, and a name for it.
auto read_blocks(const std::map<std::string, std::string>& flags,
                 std::vector<ledger::block>& blocks, std::string& source) -> void {
    if (const auto it = flags.find("export"); it != flags.end()) {
        auto [manifest, read] = ledger::read_export_directory(it->second);
        blocks = std::move(read);
        source = "export " + it->second;
    } else {
        const auto& dir = flags.at("segments");
        blocks = ledger::read_segment_directory(dir);
        source = "segments " + dir;
    }
}

auto verify(const std::map<std::string, std::string>& flags) -> int {
    std::optional<ledger::saved_digest> saved;
    if (const auto it = flags.find("digest"); it != flags.end()) {
        saved = load_digest(flags, it->second);
    }
    std::vector<ledger::block> blocks;
    std::string source;
    if (!flags.contains("export") && !flags.contains("segments")) {
        throw usage_error("verify needs --export or --segments");
    }
    try {
        read_blocks(flags, blocks, source);
    } catch (const ledger::ledger_segment_error& e) {
        std::fprintf(stderr, "FAILED: %s\n", e.what());
        return 1;
    } catch (const ledger::ledger_format_error& e) {
        std::fprintf(stderr, "FAILED: %s\n", e.what());
        return 1;
    } catch (const ledger::ion_value_error& e) {
        std::fprintf(stderr, "FAILED: not a readable journal: %s\n", e.what());
        return 1;
    }
    const std::uint64_t first = blocks.empty() ? 0 : blocks.front().seq;
    ledger::journal_verifier v(first, saved);
    for (const auto& b : blocks) {
        if (!v.add(b)) {
            break;
        }
    }
    if (!v.finish()) {
        const auto& f = *v.failure();
        if (f.seq) {
            std::fprintf(stderr, "FAILED at block %llu: %s\n",
                         static_cast<unsigned long long>(*f.seq), f.reason.c_str());
        } else {
            std::fprintf(stderr, "FAILED: %s\n", f.reason.c_str());
        }
        return 1;
    }
    std::printf("verified %llu blocks from %s", static_cast<unsigned long long>(v.blocks()),
                source.c_str());
    if (!blocks.empty()) {
        std::printf(" (%llu-%llu)", static_cast<unsigned long long>(first),
                    static_cast<unsigned long long>(first + v.blocks() - 1));
    }
    if (v.digest_checked()) {
        std::printf("; digest at block %llu matches", static_cast<unsigned long long>(saved->tip));
    } else if (const auto d = v.digest()) {
        std::printf("; digest %s", ledger::to_hex(*d).c_str());
    }
    std::printf("\n");
    return 0;
}

auto prove(const std::map<std::string, std::string>& flags) -> int {
    const auto proof_it = flags.find("proof");
    const auto digest_it = flags.find("digest");
    if (proof_it == flags.end() || digest_it == flags.end()) {
        throw usage_error("prove needs --proof and --digest");
    }
    const auto proof = ledger::proof_document::from_ion(read_ion_file(proof_it->second));
    const auto saved = load_digest(flags, digest_it->second);
    if (const auto why = ledger::check_proof(proof, saved)) {
        std::fprintf(stderr, "FAILED: %s\n", why->c_str());
        return 1;
    }
    if (proof.blk) {
        std::printf("block %llu is in the ledger at digest tip %llu\n",
                    static_cast<unsigned long long>(proof.blk->seq),
                    static_cast<unsigned long long>(saved.tip));
    } else {
        std::printf("revision %s/%s version %lld is in the ledger at digest tip %llu\n",
                    proof.rev->table.c_str(), proof.rev->id.c_str(),
                    static_cast<long long>(proof.rev->version),
                    static_cast<unsigned long long>(saved.tip));
    }
    return 0;
}

}  // namespace

auto main(int argc, char** argv) -> int {
    const std::string_view command = argc > 1 ? std::string_view(argv[1]) : std::string_view();
    if (command == "--help" || command == "help") {
        std::fputs(usage.data(), stdout);
        return 0;
    }
    try {
        const auto flags = parse_flags(argc, argv);
        if (command == "verify") {
            return verify(flags);
        }
        if (command == "prove") {
            return prove(flags);
        }
        throw usage_error(command.empty() ? "no command" : "unknown command");
    } catch (const signature_failure& e) {
        std::fprintf(stderr, "FAILED: %s\n", e.what());
        return 1;
    } catch (const usage_error& e) {
        std::fprintf(stderr, "ledger_ctl: %s\n\n%s", e.what(), usage.data());
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ledger_ctl: %s\n", e.what());
        return 2;
    }
}
