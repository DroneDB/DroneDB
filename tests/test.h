#ifndef TEST_H
#define TEST_H

#include "gtest/gtest.h"
#define TEST_NAME (std::string(::testing::UnitTest::GetInstance()->current_test_info()->test_case_name()) + \
                   "-" +                                                                                    \
                   std::string(testing::UnitTest::GetInstance()->current_test_info()->name()))


#define MANUAL_TEST(test_suite_name, test_name) TEST(test_suite_name, DISABLED_##test_name)

#endif // TEST_H
