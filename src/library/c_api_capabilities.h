/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DDB_C_API_CAPABILITIES_H
#define DDB_C_API_CAPABILITIES_H

// What the loaded library says about its own C API (DDBGetCapabilities, XI-01): the ABI version,
// the error codes and reasons, the exports that honour a cancel token and the exported symbols.
// Private to the C API (ddb_capi): nothing here is a DDB* export.

namespace ddb::capi {

// The JSON of DDBGetCapabilities() (ddb.h documents its members), built on the first call and
// owned by the library for the life of the process: the same pointer on every call and thread.
// nullptr only when the process ran out of memory while building it (the next call builds it
// again). Never throws, never changes the last error.
const char* capabilities() noexcept;

}  // namespace ddb::capi

#endif  // DDB_C_API_CAPABILITIES_H
