/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GDAL_WRITE_FAILURE_H
#define GDAL_WRITE_FAILURE_H

#include <functional>

namespace ddb_test {
// Runs operation() while the GDAL vector driver driverName fails its writes with ENOSPC: at
// creation, or at the final flush when onClose is true. Shared by the unit and the integration
// tests (build_retry_test.cpp, pending_test.cpp).
void withVectorWriteFailure(const char* driverName, bool onClose, const std::function<void()>& operation);
} // namespace ddb_test

#endif // GDAL_WRITE_FAILURE_H
