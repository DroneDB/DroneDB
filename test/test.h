#ifndef TEST_H
#define TEST_H

#include "gtest/gtest.h"
#define TEST_NAME (std::string(::testing::UnitTest::GetInstance()->current_test_info()->test_case_name()) + \
                    "-" + \
                   std::string(testing::UnitTest::GetInstance()->current_test_info()->name()))

#endif // TEST_H
