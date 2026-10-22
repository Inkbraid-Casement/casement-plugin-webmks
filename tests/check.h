// A minimal test runner: TEST(name) { CHECK(...); REQUIRE(...); }
// Run all tests, or those whose name contains the first argument.

#pragma once

#include <cstdio>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace check {

struct Test {
    const char* name;
    void (*fn)();
};

inline std::vector<Test>& registry() {
    static std::vector<Test> tests;
    return tests;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Register {
    Register(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

struct Abort : std::exception {};

template <class A, class B>
std::string describe(const A& a, const B& b) {
    std::ostringstream s;
    if constexpr (requires(std::ostream& o) { o << a; o << b; }) s << a << " vs " << b;
    return s.str();
}

inline void fail(const char* file, int line, const char* what, const std::string& detail) {
    ++failures();
    std::fprintf(stderr, "%s:%d: failed: %s %s\n", file, line, what, detail.c_str());
}

inline int run(int argc, char** argv) {
    std::string_view filter = argc > 1 ? argv[1] : "";
    int ran = 0, failed = 0;
    for (const auto& t : registry()) {
        if (!filter.empty() && std::string_view(t.name).find(filter) == std::string_view::npos) continue;
        int before = failures();
        try {
            t.fn();
        } catch (const Abort&) {
        } catch (const std::exception& e) {
            fail(t.name, 0, "exception", e.what());
        }
        ++ran;
        bool ok = failures() == before;
        if (!ok) ++failed;
        std::fprintf(stderr, "test %s ... %s\n", t.name, ok ? "ok" : "FAILED");
    }
    std::fprintf(stderr, "%d tests, %d failed\n", ran, failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace check

#define CHECK_CONCAT2(a, b) a##b
#define CHECK_CONCAT(a, b) CHECK_CONCAT2(a, b)
#define TEST(name)                                                  \
    static void name();                                             \
    static ::check::Register CHECK_CONCAT(register_, name)(#name, name); \
    static void name()

#define CHECK(cond) \
    ((cond) ? (void)0 : ::check::fail(__FILE__, __LINE__, #cond, ""))
#define CHECK_EQ(a, b) \
    (((a) == (b)) ? (void)0 : ::check::fail(__FILE__, __LINE__, #a " == " #b, ::check::describe((a), (b))))
#define REQUIRE(cond)                                                \
    do {                                                             \
        if (!(cond)) {                                               \
            ::check::fail(__FILE__, __LINE__, #cond, "(required)"); \
            throw ::check::Abort();                                  \
        }                                                            \
    } while (0)

#define CHECK_MAIN \
    int main(int argc, char** argv) { return ::check::run(argc, argv); }
