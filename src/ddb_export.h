/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DDB_EXPORT_H
#define DDB_EXPORT_H

#ifdef _WIN32
    #define DDB_DLL   __declspec(dllexport)
#else
    #define DDB_DLL
#endif // _WIN32

#endif // DDB_EXPORT_H
