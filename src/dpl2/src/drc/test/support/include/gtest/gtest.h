#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace testing {

struct TestCase
{
  std::string suite;
  std::string name;
  std::function<void()> body;
};

inline std::vector<TestCase>& registry()
{
  static std::vector<TestCase> tests;
  return tests;
}

inline int& failureCount()
{
  static int failures = 0;
  return failures;
}

class Registrar
{
 public:
  Registrar(const char* suite, const char* name, std::function<void()> body)
  {
    registry().push_back({suite, name, std::move(body)});
  }
};

class ScopedTrace
{
 public:
  explicit ScopedTrace(const char*) {}
};

inline void reportFailure(const char* file,
                          int line,
                          const std::string& expression)
{
  ++failureCount();
  std::cerr << file << ':' << line << ": failure: " << expression << '\n';
}

inline int runAllTests()
{
  int failedTests = 0;
  for (const TestCase& test : registry()) {
    const int before = failureCount();
    try {
      test.body();
    } catch (const std::exception& error) {
      reportFailure(__FILE__, __LINE__, error.what());
    } catch (...) {
      reportFailure(__FILE__, __LINE__, "unknown exception");
    }
    if (failureCount() == before) {
      std::cout << "[       OK ] " << test.suite << '.' << test.name << '\n';
    } else {
      ++failedTests;
      std::cout << "[  FAILED  ] " << test.suite << '.' << test.name << '\n';
    }
  }
  std::cout << registry().size() << " test(s), " << failedTests
            << " failure(s)\n";
  return failedTests == 0 ? 0 : 1;
}

}  // namespace testing

#define GTEST_JOIN_INNER(left, right) left##right
#define GTEST_JOIN(left, right) GTEST_JOIN_INNER(left, right)

#define TEST(suite, name)                                                   \
  static void GTEST_JOIN(suite##_##name, _body)();                          \
  static ::testing::Registrar GTEST_JOIN(suite##_##name, _registrar)(       \
      #suite, #name, GTEST_JOIN(suite##_##name, _body));                    \
  static void GTEST_JOIN(suite##_##name, _body)()

#define EXPECT_TRUE(expression)                                             \
  do {                                                                      \
    if (!(expression)) {                                                    \
      ::testing::reportFailure(__FILE__, __LINE__, #expression);            \
    }                                                                       \
  } while (false)

#define EXPECT_FALSE(expression) EXPECT_TRUE(!(expression))

#define ASSERT_EQ(left, right)                                              \
  do {                                                                      \
    const auto& gtest_left = (left);                                        \
    const auto& gtest_right = (right);                                      \
    if (!(gtest_left == gtest_right)) {                                     \
      std::ostringstream gtest_message;                                     \
      gtest_message << #left << " == " << #right << " (" << gtest_left     \
                    << " vs " << gtest_right << ')';                        \
      ::testing::reportFailure(                                             \
          __FILE__, __LINE__, gtest_message.str());                         \
      return;                                                               \
    }                                                                       \
  } while (false)

#define SCOPED_TRACE(message)                                               \
  ::testing::ScopedTrace GTEST_JOIN(gtest_trace_, __LINE__)(message)
