#ifndef TEST_H
#define TEST_H

#include "gtest/gtest.h"
#define TEST_NAME (std::string(::testing::UnitTest::GetInstance()->current_test_info()->test_case_name()) + \
                   "-" +                                                                                    \
                   std::string(testing::UnitTest::GetInstance()->current_test_info()->name()))


#define MANUAL_TEST(test_suite_name, test_name) TEST(test_suite_name, DISABLED_##test_name)
// Like MANUAL_TEST but for TEST_F fixture-based suites.
// Using TEST inside a TEST_F suite causes a gtest compile-time error.
#define MANUAL_TEST_F(test_fixture, test_name) TEST_F(test_fixture, DISABLED_##test_name)

#endif // TEST_H
