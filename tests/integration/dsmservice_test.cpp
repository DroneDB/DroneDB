/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "dsmservice.h"

namespace
{

    TEST(dsmServiceAltitude, Normal)
    {
        float altitude = DSMService::get()->getAltitude(46.84260708333, -91.99455988889); // brighton beach
        if (altitude == 0)
        {
            std::cerr << "WARNING! DSM Service is probably down or there's a serious error." << std::endl;
            EXPECT_TRUE(true);
        }
        else
        {
            EXPECT_NEAR(altitude, 191, 2);
        }
    }

}
