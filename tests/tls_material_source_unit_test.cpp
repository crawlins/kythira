// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Feature: grpc-tls-reload
//
// Unit tests for the transport-neutral TLS material sources
// (.kiro/specs/grpc-tls-reload/, Requirement 5, Task 2): the static and
// file-backed sources, publish/subscribe ordering, subscriptions that wait
// for an in-flight callback, a failed refresh that keeps the old material,
// and the file source's poll thread surviving failures and joining on stop.

#define BOOST_TEST_MODULE TlsMaterialSourceUnitTest
#include <boost/test/unit_test.hpp>

#include <raft/certificate_authority.hpp>
#include <raft/tls_material_source.hpp>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

auto issue(raft::testing::certificate_authority& ca, const std::string& cn)
    -> raft::testing::pem_material {
    raft::testing::leaf_certificate_options opts;
    opts.subject.common_name = cn;
    opts.dns_names = {cn};
    return ca.issue(opts);
}

auto material_of(const raft::testing::pem_material& leaf, const std::string& roots)
    -> kythira::tls_material {
    return {.certificate_chain_pem = leaf.certificate_pem,
            .private_key_pem = leaf.private_key_pem,
            .root_certificates_pem = roots};
}

auto roots_only(const std::string& roots) -> kythira::tls_material {
    kythira::tls_material m;
    m.root_certificates_pem = roots;
    return m;
}

// A scratch directory holding chain.pem, key.pem and roots.pem, removed when
// the test ends. write() replaces each file atomically, the way an external
// renewal agent is expected to.
struct temp_material_dir {
    std::filesystem::path dir;
    temp_material_dir() {
        char tmpl[] = "/tmp/kythira_tls_material_XXXXXX";
        BOOST_REQUIRE(::mkdtemp(tmpl) != nullptr);
        dir = tmpl;
    }
    ~temp_material_dir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    [[nodiscard]] auto paths() const -> kythira::tls_material_paths {
        return {.certificate_chain_path = (dir / "chain.pem").string(),
                .private_key_path = (dir / "key.pem").string(),
                .root_certificates_path = (dir / "roots.pem").string()};
    }
    auto replace(const std::string& name, const std::string& contents) const -> void {
        auto tmp = dir / (name + ".tmp");
        std::ofstream(tmp, std::ios::binary | std::ios::trunc) << contents;
        std::filesystem::rename(tmp, dir / name);
    }
    auto write(const kythira::tls_material& m) const -> void {
        replace("chain.pem", m.certificate_chain_pem);
        replace("key.pem", m.private_key_pem);
        replace("roots.pem", m.root_certificates_pem);
    }
};

// Polls `pred` until it holds or `deadline` passes.
template<typename Pred>
auto eventually(Pred pred, std::chrono::milliseconds deadline = 5s) -> bool {
    auto end = std::chrono::steady_clock::now() + deadline;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

}  // namespace

BOOST_AUTO_TEST_CASE(validation_rejects_incomplete_or_mismatched_material) {
    raft::testing::certificate_authority ca;
    auto a = issue(ca, "node-a");
    auto b = issue(ca, "node-b");
    const auto& roots = ca.root_certificate_pem();

    BOOST_CHECK_NO_THROW(kythira::validate_tls_material(material_of(a, roots), "t"));
    BOOST_CHECK_NO_THROW(kythira::validate_tls_material(roots_only(roots), "t"));
    BOOST_CHECK_NO_THROW(kythira::validate_tls_material(material_of(a, ""), "t"));

    auto mismatched = material_of(a, roots);
    mismatched.private_key_pem = b.private_key_pem;
    BOOST_CHECK_THROW(kythira::validate_tls_material(mismatched, "t"), std::invalid_argument);

    auto no_key = material_of(a, roots);
    no_key.private_key_pem.clear();
    BOOST_CHECK_THROW(kythira::validate_tls_material(no_key, "t"), std::invalid_argument);

    BOOST_CHECK_THROW(kythira::validate_tls_material({}, "t"), std::invalid_argument);
    BOOST_CHECK_THROW(kythira::validate_tls_material(roots_only("not pem"), "t"),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(static_source_publishes_once_and_refresh_is_a_no_op) {
    raft::testing::certificate_authority ca;
    auto a = issue(ca, "node-a");
    kythira::static_tls_material_source source(material_of(a, ca.root_certificate_pem()));

    BOOST_TEST(source.generation() == 1U);
    BOOST_TEST(source.current()->certificate_chain_pem == a.certificate_pem);
    BOOST_TEST(!source.self_refreshing());

    std::atomic<int> calls{0};
    auto sub = source.subscribe([&](auto, auto) { ++calls; });
    BOOST_CHECK_NO_THROW(source.refresh());
    BOOST_TEST(source.generation() == 1U);
    BOOST_TEST(calls.load() == 0);

    auto b = issue(ca, "node-b");
    auto bad = material_of(a, "");
    bad.private_key_pem = b.private_key_pem;
    BOOST_CHECK_THROW(kythira::static_tls_material_source{bad}, std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(file_source_refresh_publishes_new_generation_to_subscribers) {
    raft::testing::certificate_authority ca;
    const auto& roots = ca.root_certificate_pem();
    auto a = issue(ca, "node-a");
    auto b = issue(ca, "node-b");
    temp_material_dir files;
    files.write(material_of(a, roots));

    kythira::file_tls_material_source source(files.paths());
    BOOST_TEST(source.generation() == 1U);
    BOOST_TEST(!source.self_refreshing());

    std::vector<std::uint64_t> seen;
    std::string seen_chain;
    auto sub =
        source.subscribe([&](std::shared_ptr<const kythira::tls_material> m, std::uint64_t gen) {
            seen.push_back(gen);
            seen_chain = m->certificate_chain_pem;
        });

    // Unchanged files: no new generation, nobody notified.
    source.refresh();
    BOOST_TEST(source.generation() == 1U);
    BOOST_TEST(seen.empty());

    files.write(material_of(b, roots));
    source.refresh();
    BOOST_TEST(source.generation() == 2U);
    BOOST_TEST(seen == std::vector<std::uint64_t>{2}, boost::test_tools::per_element());
    BOOST_TEST(seen_chain == b.certificate_pem);

    // After unsubscribing, later publishes are not delivered.
    sub.reset();
    BOOST_TEST(!sub.active());
    files.write(material_of(a, roots));
    source.refresh();
    BOOST_TEST(source.generation() == 3U);
    BOOST_TEST(seen.size() == 1U);
}

// A torn or bad write is refused: the old material stays current, nobody is
// notified, and the failure is reported.
BOOST_AUTO_TEST_CASE(file_source_failed_refresh_keeps_previous_material) {
    raft::testing::certificate_authority ca;
    const auto& roots = ca.root_certificate_pem();
    auto a = issue(ca, "node-a");
    auto b = issue(ca, "node-b");
    temp_material_dir files;
    files.write(material_of(a, roots));

    std::atomic<int> reported{0};
    kythira::file_tls_material_source source(files.paths(), std::nullopt,
                                             [&](std::string_view) { ++reported; });
    std::atomic<int> calls{0};
    auto sub = source.subscribe([&](auto, auto) { ++calls; });

    // Half an update: b's certificate with a's key.
    files.replace("chain.pem", b.certificate_pem);
    BOOST_CHECK_THROW(source.refresh(), std::invalid_argument);
    BOOST_TEST(source.generation() == 1U);
    BOOST_TEST(source.current()->certificate_chain_pem == a.certificate_pem);
    BOOST_TEST(calls.load() == 0);
    BOOST_TEST(reported.load() == 1);
    BOOST_TEST(source.failure_count() == 1U);

    // A missing file is a failure too, not empty material.
    std::filesystem::remove(files.paths().private_key_path);
    BOOST_CHECK_THROW(source.refresh(), std::runtime_error);
    BOOST_TEST(source.current()->certificate_chain_pem == a.certificate_pem);
    BOOST_TEST(reported.load() == 2);

    // The rest of the update arrives: now it applies.
    files.replace("key.pem", b.private_key_pem);
    source.refresh();
    BOOST_TEST(source.generation() == 2U);
    BOOST_TEST(calls.load() == 1);
}

BOOST_AUTO_TEST_CASE(file_source_construction_fails_on_unreadable_files) {
    temp_material_dir files;
    BOOST_CHECK_THROW(kythira::file_tls_material_source{files.paths()}, std::runtime_error);
}

// Once reset() returns, the callback is neither running nor about to start.
BOOST_AUTO_TEST_CASE(unsubscribe_waits_for_an_in_flight_callback) {
    raft::testing::certificate_authority ca;
    const auto& roots = ca.root_certificate_pem();
    auto a = issue(ca, "node-a");
    auto b = issue(ca, "node-b");
    temp_material_dir files;
    files.write(material_of(a, roots));
    kythira::file_tls_material_source source(files.paths());

    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
    auto sub = source.subscribe([&](auto, auto) {
        entered = true;
        std::this_thread::sleep_for(200ms);
        finished = true;
    });

    files.write(material_of(b, roots));
    std::thread publisher([&] { source.refresh(); });
    BOOST_REQUIRE(eventually([&] { return entered.load(); }));
    sub.reset();
    BOOST_TEST(finished.load());
    publisher.join();

    // And a callback may drop its own subscription without deadlocking.
    kythira::tls_material_source::subscription self;
    std::atomic<int> self_calls{0};
    self = source.subscribe([&](auto, auto) {
        ++self_calls;
        self.reset();
    });
    files.write(material_of(a, roots));
    source.refresh();
    files.write(material_of(b, roots));
    source.refresh();
    BOOST_TEST(self_calls.load() == 1);
}

// A subscription may outlive its source.
BOOST_AUTO_TEST_CASE(subscription_outliving_its_source_is_safe) {
    raft::testing::certificate_authority ca;
    kythira::tls_material_source::subscription sub;
    {
        auto source = std::make_shared<kythira::static_tls_material_source>(
            material_of(issue(ca, "node-a"), ca.root_certificate_pem()));
        sub = source->subscribe([](auto, auto) {});
        BOOST_TEST(sub.active());
    }
    BOOST_TEST(!sub.active());
    BOOST_CHECK_NO_THROW(sub.reset());
}

// Many publishers and many subscribers at once: every subscriber sees
// generations in increasing order and ends on the last one.
BOOST_AUTO_TEST_CASE(concurrent_refreshes_notify_in_generation_order) {
    raft::testing::certificate_authority ca;
    const auto& roots = ca.root_certificate_pem();
    auto a = issue(ca, "node-a");
    auto b = issue(ca, "node-b");
    temp_material_dir files;
    files.write(material_of(a, roots));
    kythira::file_tls_material_source source(files.paths());

    constexpr int kSubscribers = 4;
    std::vector<std::vector<std::uint64_t>> seen(kSubscribers);
    std::vector<kythira::tls_material_source::subscription> subs;
    for (int i = 0; i < kSubscribers; ++i) {
        subs.push_back(source.subscribe([&seen, i](auto, std::uint64_t gen) {
            seen[static_cast<std::size_t>(i)].push_back(gen);
        }));
    }

    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (int i = 0; i < 40; ++i) {
            files.write(material_of(i % 2 == 0 ? b : a, roots));
        }
        stop = true;
    });
    std::vector<std::thread> refreshers;
    for (int t = 0; t < 4; ++t) {
        refreshers.emplace_back([&] {
            while (!stop.load()) {
                try {
                    source.refresh();
                } catch (const std::exception&) {
                    // A read that caught the writer mid-update is refused.
                }
            }
        });
    }
    writer.join();
    for (auto& t : refreshers) {
        t.join();
    }
    // The writer may end on the initial material, and the refreshers may
    // have caught nothing but torn reads, so finish on material nobody has
    // seen yet: that publish is guaranteed.
    files.write(material_of(issue(ca, "node-c"), roots));
    source.refresh();

    for (const auto& s : seen) {
        BOOST_TEST(std::is_sorted(s.begin(), s.end()));
        BOOST_TEST((std::adjacent_find(s.begin(), s.end()) == s.end()));
        BOOST_REQUIRE(!s.empty());
        BOOST_TEST(s.back() == source.generation());
    }
}

// The poll thread notices atomically replaced files, keeps running through
// a bad write, and joins on stop.
BOOST_AUTO_TEST_CASE(file_source_polling_reloads_and_survives_failures) {
    raft::testing::certificate_authority ca;
    const auto& roots = ca.root_certificate_pem();
    auto a = issue(ca, "node-a");
    auto b = issue(ca, "node-b");
    temp_material_dir files;
    files.write(material_of(a, roots));

    std::atomic<int> reported{0};
    kythira::file_tls_material_source source(files.paths(), 20ms,
                                             [&](std::string_view) { ++reported; });
    BOOST_TEST(source.self_refreshing());

    files.write(material_of(b, roots));
    BOOST_REQUIRE(eventually([&] { return source.generation() == 2U; }));
    BOOST_TEST(source.current()->certificate_chain_pem == b.certificate_pem);

    // A mismatched pair is refused on every poll until it is fixed.
    files.replace("chain.pem", a.certificate_pem);
    BOOST_REQUIRE(eventually([&] { return reported.load() >= 2; }));
    BOOST_TEST(source.generation() == 2U);
    files.replace("key.pem", a.private_key_pem);
    BOOST_REQUIRE(eventually([&] { return source.generation() == 3U; }));
    BOOST_TEST(source.current()->certificate_chain_pem == a.certificate_pem);

    source.stop_polling();
    BOOST_TEST(!source.self_refreshing());
    files.write(material_of(b, roots));
    std::this_thread::sleep_for(100ms);
    BOOST_TEST(source.generation() == 3U);
}
