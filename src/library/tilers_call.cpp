/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "tilers_call.h"

#include <string>
#include <utility>

#include "cancellation.h"
#include "exceptions.h"
#include "logger.h"

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

    void checkBuild(ddbtErr rc, const std::string &what,
                    const std::function<void(const std::string &)> &throwUnbuildable)
    {
        if (rc == DDBT_ERR_NO_SPACE)
            throw RetryableBuildException(what + " ran out of space: " + std::string(ddbtLastError()),
                                          RetryableBuildReason::NoSpaceOnDevice);
        cancel::throwIfCanceled();
        if (rc == DDBT_ERR_INVALIDARGS || rc == DDBT_ERR_NOT_IMPLEMENTED)
            throwUnbuildable(ddbtLastError());
        check(rc);
    }

    BuildProgress::BuildProgress(std::string label, std::function<void(double)> report)
        : label_(std::move(label)), report_(std::move(report))
    {
    }

    void BuildProgress::forward(double fraction, const char *, void *self)
    {
        static_cast<BuildProgress *>(self)->onProgress(fraction);
    }

    void BuildProgress::onProgress(double fraction)
    {
        const int pct = static_cast<int>(fraction * 100.0);
        int prev = lastPct_.load();
        if (pct < prev + 1 || !lastPct_.compare_exchange_strong(prev, pct))
            return;
        if (report_)
            report_(fraction);
        if (pct / 5 != prev / 5)
        {
            LOGD << label_ << " progress: " << pct << "%";
        }
    }
} // namespace ddb::tilers
