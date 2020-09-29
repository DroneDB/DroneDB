/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "registryutils.h"
#include "exceptions.h"
#include "constants.h"
#include "utils.h"

namespace ddb{

TagComponents RegistryUtils::parseTag(const std::string &tag, bool useInsecureRegistry){
    std::string t = tag;
    utils::trim(t);
    utils::toLower(t);

    size_t pos = t.rfind("/");
    if (pos == std::string::npos) throw InvalidArgsException("Invalid tag: " + tag + " is missing dataset name");

    TagComponents res;
    res.dataset = t.substr(pos + 1, t.length() - 1);
    t = t.substr(0, t.length() - res.dataset.length() - 1);

    pos = t.rfind("/");
    bool useDefaultRegistry = false;

    if (pos == std::string::npos){
        res.organization = t;
        useDefaultRegistry = true;
    }else{
        res.organization = t.substr(pos + 1, t.length() - 1);
    }
    t = t.substr(0, t.length() - res.organization.length() - 1);

    if (t.empty() || useDefaultRegistry){
        // Use default registry URL
        res.registryUrl = std::string(useInsecureRegistry ? "http://" : "https://") + DEFAULT_REGISTRY;
    }else{
        // TODO: should we validate the URL more throughly?
        bool hasProto = (t.find("http://") == 0 || t.find("https://") == 0) ;
        if (hasProto) res.registryUrl = t;
        else res.registryUrl = (useInsecureRegistry ? "http://" : "https://") + t;
    }

    return res;
}

Registry RegistryUtils::createFromTag(const std::string &tag, bool useInsecureRegistry){
    auto tc = parseTag(tag, useInsecureRegistry);
    return Registry(tc.registryUrl);
}

std::string TagComponents::tagWithoutUrl() const{
    return organization + "/" + dataset;
}

}
