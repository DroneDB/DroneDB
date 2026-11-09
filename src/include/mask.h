/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef MASK_H
#define MASK_H

#include <string>


namespace ddb {

/**
 * @brief Writes output, the raster input with its black (or white, or color) borders masked.
 *
 * Before it reads anything: an output that is the input is refused (InvalidArgsException),
 * and an output that exists unless overwrite (OutputExistsException). The mask is written in a
 * private folder beside output (raster::makePrivateFolder) and replaces an existing output in one
 * rename once it is complete: a failed or canceled run leaves the existing output as it was. A
 * stale external mask (`<output>.msk`) goes with the replaced output.
 */
void maskBorders(const std::string &input,
                 const std::string &output,
                 int nearDist = 15,
                 bool white = false,
                 const std::string &color = "",
                 bool overwrite = false);

}

#endif  // MASK_H
