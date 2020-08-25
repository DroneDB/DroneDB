/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DDB_H
#define DDB_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DDBErr {
    DDBERR_NONE = 0, // No error
    DDBERR_GENERIC = 1 // Generic app exception
}

extern char ddbLastError[255];

/** Get the last error message
 * @return last error message */
DDB_DLL const char* DDBGetLastError();
void DDBSetLastError(const char *err);

/** This must be called as the very first function
 * of every DDB process/program
 * @param verbose whether the program should output log messages to stdout */
DDB_DLL void DDBRegisterProcess(int verbose = 0);

/** Get library version */
DDB_DLL const char* DDBGetVersion();

/** Initialize a DroneDB database
 * @param directory Path to directory where to initialize the database
 * @return */
DDB_DLL DDBErr *DDBInit(const char *directory, char **outPath = NULL);

#ifdef __cplusplus
}
#endif

#endif // DDB_H
