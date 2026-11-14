/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TILERS_CALL_H
#define TILERS_CALL_H

#include <cstdint>
#include <memory>

#include "ddbt/ddbt.h"

namespace ddb::tilers
{
    // Throws the libddb exception of a failed ddb-tilers call, with its message (ddbtLastError):
    // InvalidArgsException for bad arguments, RefusedArgumentException for one the input cannot
    // honour (a formula the raster cannot compute), FSException for a missing input, CanceledException,
    // GDALException for anything else (what libddb's own raster code throws for a raster it cannot
    // read or render). Nothing when rc is DDBT_ERR_NONE.
    void check(ddbtErr rc);

    // A buffer or string ddb-tilers returned, released with ddbtFree
    struct FreeBuffer
    {
        void operator()(void *p) const noexcept { ddbtFree(p); }
    };
    using Bytes = std::unique_ptr<uint8_t, FreeBuffer>;
    using String = std::unique_ptr<char, FreeBuffer>;
} // namespace ddb::tilers

#endif // TILERS_CALL_H
