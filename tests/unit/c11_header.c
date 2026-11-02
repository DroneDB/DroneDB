/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* ddb.h is valid C11. tests/CMakeLists.txt compiles this file as C11 with
 * -Wall -Wextra -Wpedantic -Wstrict-prototypes -Werror (MSVC: /std:c11 /W4 /WX), so a C++-only
 * construct in ddb.h (a default argument, an enum used without its typedef, bool without
 * <stdbool.h>, an empty parameter list) fails the build of ddb_unit_tests. ddb.h also checks the
 * layout of its structs with static assertions, here in C. c_header_test.cpp calls the functions
 * below, so the C declarations link against the library as well. */

#include "ddb.h"

#include "c11_header.h"

DDBTestCLayout ddbTestCLayout(void)
{
    DDBTestCLayout layout;
    layout.boolSize = sizeof(bool);
    layout.errSize = sizeof(DDBErr);
    layout.addOptionsSize = sizeof(DDBAddOptions);
    layout.addOptionsStopOnError = offsetof(DDBAddOptions, stopOnError);
    layout.addOptionsMaxConflictRetries = offsetof(DDBAddOptions, maxConflictRetries);
    layout.addHintSize = sizeof(DDBAddHint);
    layout.addHintSha256 = offsetof(DDBAddHint, sha256);
    layout.addHintFileSize = offsetof(DDBAddHint, size);
    layout.addHintMtime = offsetof(DDBAddHint, mtime);
    return layout;
}

static int keepGoing(double fraction, const char *phase, void *userData)
{
    (void)fraction;
    (void)phase;
    (void)userData;
    return 0;
}

int ddbTestCCalls(void)
{
    const uint32_t abi = ((uint32_t)DDB_ABI_MAJOR << 16) | (uint32_t)DDB_ABI_MINOR;
    DDBProgressCallback progress = keepGoing;
    DDBAddOptions options = {false, true, 2};
    char *out = NULL;
    bool verified = true;

    /* The exempt accessors */
    if (DDBGetAbiVersion() != abi)
        return 1;
    if (DDBGetVersion() == NULL || DDBGetCapabilities() == NULL)
        return 2;
    if (DDBFree(NULL) != DDBERR_NONE || DDBVSIFree(NULL) != DDBERR_NONE)
        return 3;

    /* A failing call: its code, a message, no reason, the output left alone */
    if (DDBNormalizePath(NULL, NULL, &out) != DDBERR_INVALIDARGS || out != NULL)
        return 4;
    if (DDBGetLastError()[0] == '\0' || DDBGetLastErrorReason()[0] != '\0')
        return 5;

    /* bool, struct and callback arguments from C */
    if (DDBVerifyPassword(NULL, "", &verified) != DDBERR_EXCEPTION || !verified)
        return 6;
    if (DDBAddWithOptions(NULL, NULL, 0, &options, &out) != DDBERR_EXCEPTION || out != NULL)
        return 7;
    if (DDBExportRaster2(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, progress, NULL) != DDBERR_EXCEPTION)
        return 8;

    /* A successful call clears the last error */
    if (DDBNormalizePath("a/b", NULL, &out) != DDBERR_NONE || out == NULL)
        return 9;
    DDBFree(out);
    if (DDBGetLastError()[0] != '\0')
        return 10;
    return 0;
}
