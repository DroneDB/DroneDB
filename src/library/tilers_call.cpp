/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "tilers_call.h"

#include <string>

#include "exceptions.h"

namespace ddb::tilers
{
    void check(ddbtErr rc)
    {
        if (rc == DDBT_ERR_NONE)
            return;
        const std::string message = ddbtLastError();
        switch (rc)
        {
        case DDBT_ERR_INVALIDARGS:
            throw InvalidArgsException(message);
        case DDBT_ERR_REFUSED:
            throw RefusedArgumentException(message);
        case DDBT_ERR_NOTFOUND:
            throw FSException(message);
        case DDBT_ERR_CANCELED:
            throw CanceledException(message);
        default:
            throw GDALException(message);
        }
    }
} // namespace ddb::tilers
