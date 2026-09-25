// A ~60-line test harness.
//
// Deliberately dependency-free: the engine has no third-party libraries, and a
// vendored framework would be larger than the code it tests. Self-registering
// TEST() blocks, expression-printing assertions, one binary, non-zero exit on
// failure — everything CI needs and nothing more.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace testing {

struct TestCase {
  const char* suite;
  const char* name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

inline int& failures() {
  static int f = 0;
  return f;
}

inline std::string& current() {
  static std::string c;
  return c;
}

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> fn) {
    registry().push_back({suite, name, std::move(fn)});
  }
};

inline void report_failure(const char* file, int line, const std::string& expr,
                           const std::string& detail) {
  ++failures();
  std::printf("  FAIL %s\n    %s:%d\n    %s\n", current().c_str(), file, line, expr.c_str());
  if (!detail.empty()) std::printf("    %s\n", detail.c_str());
}

template <typename T>
std::string show(const T& v) {
  if constexpr (std::is_convertible_v<T, std::string>) {
    return "\"" + std::string(v) + "\"";
  } else if constexpr (std::is_floating_point_v<T>) {
    return std::to_string(v);
  } else {
    return std::to_string(v);
  }
}

inline int run_all() {
  int passed = 0;
  const char* suite = nullptr;
  for (const TestCase& t : registry()) {
    if (suite == nullptr || std::string(suite) != t.suite) {
      suite = t.suite;
      std::printf("\n[%s]\n", suite);
    }
    current() = std::string(t.suite) + " / " + t.name;
    const int before = failures();
    t.fn();
    if (failures() == before) {
      ++passed;
      std::printf("  ok   %s\n", t.name);
    }
  }
  std::printf("\n%d passed, %d failed, %zu total\n", passed, failures(), registry().size());
  return failures() == 0 ? 0 : 1;
}

}  // namespace testing

#define TEST(suite, name)                                                          \
  static void suite##_##name();                                                    \
  static ::testing::Registrar reg_##suite##_##name(#suite, #name, suite##_##name); \
  static void suite##_##name()

#define CHECK(expr)                                                     \
  do {                                                                  \
    if (!(expr)) ::testing::report_failure(__FILE__, __LINE__, #expr, ""); \
  } while (0)

#define CHECK_EQ(a, b)                                                              \
  do {                                                                              \
    const auto _a = (a);                                                            \
    const auto _b = (b);                                                            \
    if (!(_a == _b)) {                                                              \
      ::testing::report_failure(__FILE__, __LINE__, #a " == " #b,                    \
                                "got " + ::testing::show(_a) +                       \
                                    ", expected " + ::testing::show(_b));            \
    }                                                                               \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                       \
  do {                                                                              \
    const double _a = static_cast<double>(a);                                        \
    const double _b = static_cast<double>(b);                                        \
    if (std::fabs(_a - _b) > (eps)) {                                                \
      ::testing::report_failure(__FILE__, __LINE__, #a " ~= " #b,                    \
                                "got " + std::to_string(_a) +                        \
                                    ", expected " + std::to_string(_b));             \
    }                                                                               \
  } while (0)
