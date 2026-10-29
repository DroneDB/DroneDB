/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gdal_metadata.h"

#include <cpl_string.h>

#include "stringutil.h"

namespace ddb::gdalmeta {

namespace {

// The domains that list the file itself (DERIVED_SUBDATASET_n_NAME=...:/path/file.tif)
bool isPathDomain(const char *domain) {
    const std::string_view name(domain);
    return name == "DERIVED_SUBDATASETS" || name == "SUBDATASETS";
}

// The value of a "KEY=VALUE" entry; an entry without '=' is a value by itself
std::string_view entryValue(std::string_view entry) {
    const auto eq = entry.find('=');
    return eq == std::string_view::npos ? entry : entry.substr(eq + 1);
}

void appendValue(std::vector<std::string> &out, std::string_view entry, std::string_view value) {
    if (!isPathOrUrl(value)) out.emplace_back(entry);
}

void appendList(std::vector<std::string> &out, CSLConstList list) {
    if (!list) return;
    for (int k = 0; list[k] != nullptr; k++) appendValue(out, list[k], entryValue(list[k]));
}

} // namespace

bool isPathOrUrl(std::string_view value) {
    while (!value.empty() && str::isSpaceChar(value.front())) value.remove_prefix(1);
    if (value.empty()) return false;
    if (value.front() == '/' || value.front() == '\\') return true;
    if (value.size() >= 2 && str::isAlphaChar(value[0]) && value[1] == ':') return true;
    return value.find("://") != std::string_view::npos;
}

bool containsToken(std::string_view text, std::string_view token) {
    if (token.empty()) return false;
    size_t i = 0;
    while (i < text.size()) {
        if (!str::isAlnumChar(text[i])) {
            i++;
            continue;
        }
        size_t end = i;
        while (end < text.size() && str::isAlnumChar(text[end])) end++;
        if (end - i == token.size()) {
            bool same = true;
            for (size_t k = 0; same && k < token.size(); k++)
                same = str::toLowerChar(text[i + k]) == str::toLowerChar(token[k]);
            if (same) return true;
        }
        i = end;
    }
    return false;
}

std::vector<std::string> collectMetadata(GDALDatasetH dataset, const CollectOptions &options) {
    std::vector<std::string> out;

    for (const auto &item : options.datasetItems) {
        const char *value = GDALGetMetadataItem(dataset, item.c_str(), nullptr);
        if (value) appendValue(out, value, value);
    }

    const CPLStringList domains(GDALGetMetadataDomainList(dataset), TRUE);
    for (int d = 0; d < domains.size(); d++) {
        if (isPathDomain(domains[d])) continue;
        appendList(out, GDALGetMetadata(dataset, domains[d]));
    }

    for (int b = 1; b <= options.bandCount; b++) {
        GDALRasterBandH band = GDALGetRasterBand(dataset, b);
        if (!band) continue;
        if (options.bandDescriptions) {
            const char *description = GDALGetDescription(band);
            if (description && description[0]) appendValue(out, description, description);
        }
        appendList(out, GDALGetMetadata(band, nullptr));
    }
    return out;
}

} // namespace ddb::gdalmeta
