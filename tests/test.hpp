#pragma once

#include <cstdio>
#include <vector>

namespace test {

struct Case {
    const char* name;
    void (*fn)();
};

std::vector<Case>& registry();
extern int failures;

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

}  // namespace test

#define TEST(name)                                                   \
    static void test_##name();                                       \
    static test::Registrar registrar_##name(#name, &test_##name);    \
    static void test_##name()

#define CHECK_EQ(a, b)                                                                 \
    do {                                                                               \
        auto va_ = (a);                                                                \
        auto vb_ = (b);                                                                \
        if (!(va_ == vb_)) {                                                           \
            ++test::failures;                                                          \
            std::printf("  %s:%d: CHECK_EQ(%s, %s) failed\n", __FILE__, __LINE__, #a, #b); \
        }                                                                              \
    } while (0)

#define CHECK(x) CHECK_EQ(static_cast<bool>(x), true)
