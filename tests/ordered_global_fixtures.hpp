// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Run a suite's Boost.Test global fixtures in the order they are listed.
//
// Why this exists
// ---------------
// Boost.Test does NOT run global fixtures in declaration order. Each
// registration macro defines a static object named `gf_<F>`:
//
//   * `BOOST_GLOBAL_FIXTURE(F)` and `BOOST_TEST_GLOBAL_CONFIGURATION(F)`
//     define a `global_configuration_impl<F>`, a `test_observer` that does
//     `new F` in `test_start()` and `delete` in `test_finish()`. The framework
//     keeps observers in a `std::set<test_observer*, priority_order>`; every
//     global configuration has priority 1, and equal priorities fall back to
//     `std::less<test_observer*>` (boost/test/impl/framework.ipp,
//     `priority_order`). Construction therefore follows the *addresses* of the
//     `gf_<F>` statics, and destruction the reverse of that.
//   * `BOOST_TEST_GLOBAL_FIXTURE(F)` defines a `global_fixture_impl<F>`,
//     which the framework keeps in a plain `std::set<global_fixture*>` — again
//     ordered by address. It constructs `F` and calls its `setup()` /
//     `teardown()` if `F` has them.
//
// Where the linker places those statics is up to the toolchain, so two
// fixtures declared A-then-B may run B-then-A. That happened to
// `aws_acm_pca_provider_real_test` in a GCC build: `CaLifetimeFixture`, the
// last fixture declared, landed at the lowest address and ran first. It ran
// before `AwsSdkFixture`, so `Aws::Client::ClientConfiguration()` segfaulted
// because `Aws::InitAPI` had not run yet. It also ran before
// `PreflightSkipFixture`, whose whole job is to exit 77 before any AWS work:
// with a slightly different layout, an un-gated run could have created
// billable CAs before the skip check fired. CI only ever saw the declared order
// by luck of layout. `-fno-toplevel-reorder` restores it for GCC, but that is
// a compiler workaround, not an ordering guarantee. (With GCC 13 and Boost
// 1.83, four fixtures registered A, B, C, D in one file run A B C D at -O0 and
// D C B A at -O2.)
//
// What this does
// --------------
// `ordered_global_fixtures<A, B, C>` is a single fixture type holding one
// object whose direct bases are (wrappers around) A, B and C, in that order.
// The C++ standard guarantees that direct bases are constructed left to right
// and destroyed in reverse ([class.base.init], [class.dtor]), so registering
// the composite once makes the order a property of the source instead of the
// link map:
//
//   using SuiteGlobalFixtures =
//       kythira::testing::ordered_global_fixtures<PreflightSkipFixture,
//                                                 FollyInitFixture,
//                                                 AwsSdkFixture>;
//   BOOST_GLOBAL_FIXTURE(SuiteGlobalFixtures);
//
// The alias is required: the macros paste their argument into an identifier,
// and a template-id containing commas would also split the macro argument.
//
// A suite must register at most one composite per registration macro kind.
// Two separate `BOOST_GLOBAL_FIXTURE` lines are back to address order.
//
// Conditional fixtures
// --------------------
// Some fixtures exist only in some builds (FollyInitFixture only on the Folly
// future backend, FiuInitFixture only with FIU_ENABLE). Alias the missing
// fixture to `no_global_fixture` in the `#else` branch so the list stays the
// same in every configuration:
//
//   #if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && ...
//   struct FollyInitFixture { ... };
//   #else
//   using FollyInitFixture = kythira::testing::no_global_fixture;
//   #endif
//
// The same type may appear more than once in the list (two absent fixtures
// both become `no_global_fixture`): each entry is wrapped in a slot tagged
// with its position, so the bases stay distinct.
//
// setup() / teardown()
// --------------------
// The composite keeps whatever contract the registration macro gives a single
// fixture:
//
//   * Under `BOOST_GLOBAL_FIXTURE` / `BOOST_TEST_GLOBAL_CONFIGURATION`, Boost
//     only constructs and deletes the composite, exactly as it did each
//     fixture. A fixture's own `setup()` / `teardown()` are not called by
//     Boost there, and are not called by the composite either.
//   * Under `BOOST_TEST_GLOBAL_FIXTURE`, Boost calls the composite's
//     `setup()` after constructing it and `teardown()` before deleting it.
//     Those call each listed fixture's `setup()` in list order and each
//     `teardown()` in reverse list order, for the fixtures that have them.
//     All constructors run before the first `setup()`, rather than
//     interleaved per fixture as separate registrations would do.
//
// The fixtures are held in a member, not inherited by the composite itself,
// so the composite's `setup()` / `teardown()` cannot accidentally override a
// fixture's virtual `teardown()` (e.g. a `signal_cleanup_target`'s).
//
// Construction failure: if a fixture's constructor throws, the fixtures after
// it are not constructed and the ones before it are destroyed in reverse.
// Separate registrations used to keep constructing the remaining fixtures.
//
// Ordering *between* the two macro kinds is Boost's own, and is fixed: every
// configuration observer's `test_start()` runs before any
// `BOOST_TEST_GLOBAL_FIXTURE` is set up, and teardown is the mirror image. A
// suite that uses both kinds registers one composite of each.

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace kythira::testing {

/// Stand-in for a global fixture that is compiled out of this build. Does
/// nothing; exists so `ordered_global_fixtures` lists stay identical across
/// `#if` branches.
struct no_global_fixture {};

namespace ordered_global_fixtures_detail {

/// Position-tagged wrapper, so the same fixture type (typically
/// `no_global_fixture`) can appear more than once in one list.
template<std::size_t I, typename F> struct slot : F {
    static_assert(std::is_class_v<F>, "a global fixture must be a class type");
};

template<typename Seq, typename... Fs> struct slots;

/// Direct bases in list order: constructed left to right, destroyed right to
/// left.
template<std::size_t... Is, typename... Fs>
struct slots<std::index_sequence<Is...>, Fs...> : slot<Is, Fs>... {};

}  // namespace ordered_global_fixtures_detail

/// One Boost.Test global fixture that constructs `Fs...` in the order listed
/// and destroys them in reverse. See the comment at the top of this file.
template<typename... Fs> class ordered_global_fixtures {
public:
    ordered_global_fixtures() = default;
    ordered_global_fixtures(const ordered_global_fixtures&) = delete;
    auto operator=(const ordered_global_fixtures&) -> ordered_global_fixtures& = delete;
    ordered_global_fixtures(ordered_global_fixtures&&) = delete;
    auto operator=(ordered_global_fixtures&&) -> ordered_global_fixtures& = delete;
    ~ordered_global_fixtures() = default;

    /// Called by Boost only under BOOST_TEST_GLOBAL_FIXTURE.
    void setup() { setup_each(std::index_sequence_for<Fs...>{}); }

    /// Called by Boost only under BOOST_TEST_GLOBAL_FIXTURE.
    void teardown() { teardown_each(std::index_sequence_for<Fs...>{}); }

private:
    template<std::size_t I> using fixture_at = std::tuple_element_t<I, std::tuple<Fs...>>;

    template<std::size_t I> auto at() -> fixture_at<I>& {
        return static_cast<ordered_global_fixtures_detail::slot<I, fixture_at<I>>&>(_fixtures);
    }

    template<std::size_t I> void setup_one() {
        if constexpr (requires(fixture_at<I>& f) { f.setup(); }) {
            at<I>().setup();
        }
    }

    template<std::size_t I> void teardown_one() {
        if constexpr (requires(fixture_at<I>& f) { f.teardown(); }) {
            at<I>().teardown();
        }
    }

    template<std::size_t... Is> void setup_each(std::index_sequence<Is...> /*unused*/) {
        (setup_one<Is>(), ...);
    }

    template<std::size_t... Is> void teardown_each(std::index_sequence<Is...> /*unused*/) {
        (teardown_one<sizeof...(Fs) - 1 - Is>(), ...);
    }

    ordered_global_fixtures_detail::slots<std::index_sequence_for<Fs...>, Fs...> _fixtures;
};

}  // namespace kythira::testing
