#pragma once

#include <functional>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

std::vector<TestCase>& registry();
extern int failures;

struct Registrar {
  Registrar(const char* name, std::function<void()> fn);
};

void fail(const char* file, int line, const std::string& message);

} // namespace testing

#define TEST(name)                                              \
  static void name();                                           \
  static ::testing::Registrar registrar_##name(#name, name);     \
  static void name()

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) ::testing::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define CHECK_EQ(actual, expected)                                                  \
  do {                                                                              \
    const long long a_ = static_cast<long long>(actual);                            \
    const long long b_ = static_cast<long long>(expected);                          \
    if (a_ != b_) {                                                                 \
      ::testing::fail(__FILE__, __LINE__,                                           \
                      std::string("CHECK_EQ(" #actual ", " #expected ") -> ") +     \
                          std::to_string(a_) + " != " + std::to_string(b_));        \
    }                                                                               \
  } while (0)
