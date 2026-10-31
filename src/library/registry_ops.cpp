/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "registry_ops.h"

#include "exceptions.h"
#include "tagmanager.h"

namespace ddb {

std::string datasetRegistryUrl(Database* db, const std::string& registry, const std::string& verb) {
    if (!registry.empty())
        return registry;

    TagManager manager(db);
    const auto tag = manager.getTag();

    if (tag.empty())
        throw IndexException("Cannot " + verb + " if no tag is specified");

    return RegistryUtils::parseTag(tag).registryUrl;
}

}  // namespace ddb
