#include <iostream>

namespace dpl2 {
#define PASS_TEST(name) std::cout << name << ": PASS" << std::endl

#define EXPECT_ASSERT 0

#define ENABLE_ASSERT  \
  if (EXPECT_ASSERT) { \
    uvAssert(0);       \
  }

#define EXPECT_EQ(obj, value)                                      \
  if ((value) != (obj)) {                                          \
    std::cout << __FILE__ << ":" << std::dec << __LINE__         \
              << " \033[31mFAIL:\033[0m expects \"" << (obj)       \
              << "\" but actual is \"" << (value) << "\"\n";       \
    ENABLE_ASSERT                                                  \
  } else {                                                         \
    std::cout << __FILE__ << ":" << std::dec << __LINE__         \
              << " \033[32mPASS\033[0m\n";                         \
  }

#define EXPECT_TRUE(value)                                         \
  if (!(value)) {                                                    \
    std::cout << __FILE__ << ":" << std::dec << __LINE__         \
              << " \033[31mFAIL:\033[0m not true\n";                 \
    ENABLE_ASSERT                                                  \
  } else {                                                         \
    std::cout << __FILE__ << ":" << std::dec << __LINE__         \
              << " \033[32mPASS\033[0m\n";                         \
  }

#define DEBUG_INFO(msg)                                              \
  std::cout << __PRETTY_FUNCTION__ << ":" << std::dec << __LINE__ << "," \
            << msg << std::endl;

}  // namespace dpl2