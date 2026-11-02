/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DDB_TEST_C11_HEADER_H
#define DDB_TEST_C11_HEADER_H

/* The functions of c11_header.c (a C11 translation unit), called by c_header_test.cpp */

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* sizeof/offsetof of the public types of ddb.h, as a C compiler lays them out */
    typedef struct
    {
        size_t boolSize;
        size_t errSize;
        size_t addOptionsSize;
        size_t addOptionsStopOnError;
        size_t addOptionsMaxConflictRetries;
        size_t addHintSize;
        size_t addHintSha256;
        size_t addHintFileSize;
        size_t addHintMtime;
    } DDBTestCLayout;

    DDBTestCLayout ddbTestCLayout(void);

    /* Calls exports of ddb.h from C; returns 0 when every call behaved as documented, otherwise
     * the number of the first check that failed */
    int ddbTestCCalls(void);

#ifdef __cplusplus
}
#endif

#endif /* DDB_TEST_C11_HEADER_H */
