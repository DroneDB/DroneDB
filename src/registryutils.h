/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef REGISTRYUTILS_H
#define REGISTRYUTILS_H

#include <string>

namespace ddb{

struct TagComponents{
    std::string registryUrl;
    std::string organization;
    std::string dataset;
};

class RegistryUtils{
public:
    static TagComponents parseTag(const std::string &tag, bool useInsecureRegistry = false);
};

}
#endif // REGISTRYUTILS_H
