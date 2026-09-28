// A dependency-free test harness. The project vendors nothing, so this is
// deliberately small: registration, assertions, and a non-zero exit code.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace arecomp::test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int count = 0;
    return count;
}

inline std::string& current_case() {
    static std::string name;
    return name;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline void report_failure(const char* file, int line, const std::string& what) {
    ++failures();
    std::fprintf(stderr, "  FAIL %s\n    at %s:%d\n    %s\n",
                 current_case().c_str(), file, line, what.c_str());
}

inline std::string hex(unsigned long long v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%llx", v);
    return buf;
}

inline int run_all() {
    int executed = 0;
    for (const Case& c : registry()) {
        current_case() = c.name;
        const int before = failures();
        c.fn();
        ++executed;
        if (failures() == before) std::printf("  ok   %s\n", c.name);
    }
    std::printf("%d case(s), %d failure(s)\n", executed, failures());
    return failures() == 0 ? 0 : 1;
}

} // namespace arecomp::test

#define ARECOMP_CONCAT_INNER(a, b) a##b
#define ARECOMP_CONCAT(a, b) ARECOMP_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                         \
    static void ARECOMP_CONCAT(arecomp_test_fn_, __LINE__)();                   \
    static ::arecomp::test::Registrar ARECOMP_CONCAT(arecomp_test_reg_, __LINE__)( \
        name, &ARECOMP_CONCAT(arecomp_test_fn_, __LINE__));                     \
    static void ARECOMP_CONCAT(arecomp_test_fn_, __LINE__)()

#define CHECK(expr)                                                             \
    do {                                                                        \
        if (!(expr))                                                            \
            ::arecomp::test::report_failure(__FILE__, __LINE__,                 \
                                            "expected: " #expr);                \
    } while (0)

#define CHECK_EQ(actual, expected)                                              \
    do {                                                                        \
        const auto a_ = (actual);                                               \
        const auto e_ = (expected);                                             \
        if (!(a_ == e_))                                                        \
            ::arecomp::test::report_failure(                                    \
                __FILE__, __LINE__,                                             \
                std::string(#actual) + " = " +                                  \
                    ::arecomp::test::hex(static_cast<unsigned long long>(a_)) + \
                    ", expected " +                                             \
                    ::arecomp::test::hex(static_cast<unsigned long long>(e_))); \
    } while (0)

#define CHECK_STR_EQ(actual, expected)                                          \
    do {                                                                        \
        const std::string a_ = (actual);                                        \
        const std::string e_ = (expected);                                      \
        if (a_ != e_)                                                           \
            ::arecomp::test::report_failure(__FILE__, __LINE__,                 \
                                            #actual " = \"" + a_ +             \
                                                "\", expected \"" + e_ + "\""); \
    } while (0)

#define ARECOMP_TEST_MAIN()                                                     \
    int main() { return ::arecomp::test::run_all(); }
