// Minimal test runner: tests register themselves with TEST(name).
#include "test.hpp"

#include <cstdio>

namespace test {

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

int failures = 0;

}  // namespace test

int main() {
    int failedCases = 0;
    for (const auto& c : test::registry()) {
        int before = test::failures;
        c.fn();
        if (test::failures != before) {
            ++failedCases;
            std::printf("FAIL %s\n", c.name);
        }
    }
    std::printf("%zu tests, %d failed\n", test::registry().size(), failedCases);
    return failedCases == 0 ? 0 : 1;
}
