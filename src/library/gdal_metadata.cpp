/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gdal_metadata.h"

#include <cpl_string.h>

#include "raster_source.h"
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

// Whether the text starts with "<scheme>://" (RFC 3986: a letter, then letters, digits, '+',
// '-' or '.'). Only a value that starts so is a URL: an XMP packet declares its namespaces with
// "http://" URIs and a description may mention a link, and both are text worth matching.
bool startsWithUrlScheme(std::string_view value) {
    if (value.empty() || !str::isAlphaChar(value.front())) return false;
    size_t i = 1;
    while (i < value.size() && (str::isAlnumChar(value[i]) || value[i] == '+' || value[i] == '-' || value[i] == '.'))
        i++;
    return value.substr(i, 3) == "://";
}

void appendValue(std::vector<std::string> &out, std::string_view entry, std::string_view value) {
    if (!isPathOrUrl(value)) out.emplace_back(entry);
}

void appendList(std::vector<std::string> &out, const std::vector<std::string> &list) {
    for (const auto &entry : list) appendValue(out, entry, entryValue(entry));
}

std::vector<std::string> stringsOf(CSLConstList list) {
    std::vector<std::string> out;
    for (int k = 0; list != nullptr && list[k] != nullptr; k++) out.emplace_back(list[k]);
    return out;
}

} // namespace

bool isPathOrUrl(std::string_view value) {
    while (!value.empty() && str::isSpaceChar(value.front())) value.remove_prefix(1);
    if (value.empty()) return false;
    if (value.front() == '/' || value.front() == '\\') return true;
    if (value.size() >= 2 && str::isAlphaChar(value[0]) && value[1] == ':') return true;
    return startsWithUrlScheme(value);
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

RasterMetadata metadataOf(GDALDatasetH dataset) {
    RasterMetadata metadata;
    const CPLStringList domains(GDALGetMetadataDomainList(dataset), TRUE);
    for (int d = 0; d < domains.size(); d++)
        metadata.domains.push_back(Domain{domains[d], stringsOf(GDALGetMetadata(dataset, domains[d]))});
    for (int b = 1; b <= GDALGetRasterCount(dataset); b++)
        metadata.bandItems.push_back(stringsOf(GDALGetMetadata(GDALGetRasterBand(dataset, b), nullptr)));
    return metadata;
}

std::optional<std::string> itemOf(const RasterMetadata &metadata, std::string_view domain, std::string_view name) {
    for (const auto &d : metadata.domains) {
        if (!EQUAL(d.name.c_str(), std::string(domain).c_str())) continue;
        for (const auto &item : d.items)
            if (item.size() > name.size() && EQUALN(item.c_str(), std::string(name).c_str(), name.size()) &&
                (item[name.size()] == '=' || item[name.size()] == ':'))
                return item.substr(name.size() + 1);
    }
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> parseNameValue(std::string_view item) {
    const size_t separator = item.find_first_of("=:");
    if (separator == std::string_view::npos) return std::nullopt;
    std::string_view name = item.substr(0, separator), value = item.substr(separator + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.remove_suffix(1);
    return std::make_pair(std::string(name), std::string(value));
}

std::vector<std::string> collectMetadata(const RasterSource &raster, const CollectOptions &options) {
    std::vector<std::string> out;
    const RasterMetadata metadata = raster.metadata();

    for (const auto &item : options.datasetItems) {
        if (const auto value = itemOf(metadata, "", item)) appendValue(out, *value, *value);
    }

    for (const auto &domain : metadata.domains) {
        if (isPathDomain(domain.name.c_str())) continue;
        appendList(out, domain.items);
    }

    for (int b = 1; b <= options.bandCount && b <= raster.bandCount(); b++) {
        if (options.bandDescriptions) {
            const std::string description = raster.description(b);
            if (!description.empty()) appendValue(out, description, description);
        }
        appendList(out, metadata.bandItems[static_cast<size_t>(b - 1)]);
    }
    return out;
}

} // namespace ddb::gdalmeta
