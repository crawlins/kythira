// **Feature: stdexec-future-backend, Property 2: Concept-Layer Folly
// Independence**
// For any compilation unit that includes only include/concepts/future.hpp,
// compilation should succeed without transitively including any Folly
// header.
// **Validates: Requirement 1.4**
//
// This file's own successful compilation *is* the test: the CMake target
// below (concepts_future_folly_independence_test, tests/CMakeLists.txt)
// deliberately does not link network_simulator or any other target that
// would add Folly's include directories to the compiler invocation — only
// the project-wide include/ directory (include_directories() in the root
// CMakeLists.txt) is available. If a regression made
// include/concepts/future.hpp depend on any Folly header, directly or
// transitively, this translation unit would fail to compile with a
// "file not found" diagnostic rather than a runtime assertion failure —
// exactly the CMake/CTest-level check Task 5's second bullet asks for,
// doubling as this task's dedicated property test.

#include <concepts/future.hpp>

#include <exception>
#include <type_traits>

// A minimal type satisfying try_type, to exercise the regenericized
// concept surface (std::exception_ptr, not folly::exception_wrapper)
// without needing any backend at all.
template<typename T> class minimal_try {
public:
    explicit minimal_try(T value) : _value(std::move(value)) {}
    explicit minimal_try(std::exception_ptr ex) : _ex(ex) {}

    [[nodiscard]] auto hasValue() const -> bool { return !_ex; }
    [[nodiscard]] auto hasException() const -> bool { return static_cast<bool>(_ex); }
    [[nodiscard]] auto exception() const -> std::exception_ptr { return _ex; }
    [[nodiscard]] auto value() -> T& { return _value; }
    [[nodiscard]] auto value() const -> const T& { return _value; }

private:
    T _value{};
    std::exception_ptr _ex;
};

static_assert(kythira::try_type<minimal_try<int>, int>,
              "minimal_try<int> must satisfy the regenericized try_type concept "
              "using only std::exception_ptr, with no Folly type involved");

static_assert(std::is_default_constructible_v<kythira::unit>);
static_assert(kythira::unit{} == kythira::unit{});

auto main() -> int {
    return 0;
}
