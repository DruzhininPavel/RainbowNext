#include "test_framework.h"

#include <cstdio>

namespace testing {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

int failures = 0;

Registrar::Registrar(const char* name, std::function<void()> fn) {
  registry().push_back({name, std::move(fn)});
}

void fail(const char* file, int line, const std::string& message) {
  ++failures;
  std::fprintf(stderr, "    %s:%d: %s\n", file, line, message.c_str());
}

} // namespace testing

int main() {
  int failedTests = 0;
  for (const testing::TestCase& test : testing::registry()) {
    const int before = testing::failures;
    std::printf("[ RUN  ] %s\n", test.name.c_str());
    test.fn();
    if (testing::failures == before) {
      std::printf("[  OK  ] %s\n", test.name.c_str());
    } else {
      std::printf("[ FAIL ] %s\n", test.name.c_str());
      ++failedTests;
    }
  }

  std::printf("\n%zu tests, %d failed assertion(s), %d failed test(s)\n",
              testing::registry().size(), testing::failures, failedTests);
  return testing::failures == 0 ? 0 : 1;
}
