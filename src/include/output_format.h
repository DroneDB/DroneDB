/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef OUTPUT_FORMAT_H
#define OUTPUT_FORMAT_H

#include <initializer_list>
#include <iosfwd>
#include <optional>
#include <string>

#include "ddb_export.h"
#include "json_fwd.h"

namespace ddb
{

    /// How a command writes its result. Hosts (the CLI, the C API) name it as text; libddb parses
    /// that text once, at its boundary, and works with the enum from there.
    enum class OutputFormat
    {
        Text,
        Json,
        GeoJson
    };

    /// "text", "json" or "geojson"
    DDB_DLL const char *outputFormatName(OutputFormat format) noexcept;

    /// The format called `name` (exactly, in lowercase); nothing for any other text
    DDB_DLL std::optional<OutputFormat> findOutputFormat(const std::string &name);

    /// The format called `name` when it is one of `allowed`; otherwise throws an
    /// InvalidArgsException "Invalid format <name>"
    DDB_DLL OutputFormat parseOutputFormat(const std::string &name,
                                           std::initializer_list<OutputFormat> allowed = {
                                               OutputFormat::Text, OutputFormat::Json, OutputFormat::GeoJson});

    /// Throws the same InvalidArgsException when `format` is not one of `allowed`
    DDB_DLL void requireOutputFormat(OutputFormat format, std::initializer_list<OutputFormat> allowed);

    /**
     * Writes a JSON array one member at a time, so a listing is never held as one document: the
     * opening text when constructed, a "," before every member but the first, the closing text
     * from finish(). The default text is a plain array; a GeoJSON feature collection passes its
     * own opening and closing.
     */
    class DDB_DLL JsonArrayWriter
    {
        std::ostream &out_;
        std::string close_;
        bool first_ = true;

    public:
        explicit JsonArrayWriter(std::ostream &out, const std::string &open = "[", std::string close = "]");

        /// The member, serialized compactly (json::dump)
        void write(const json &member);

        /// The closing text. Not called when the listing fails half way: the output then stays
        /// unterminated, as the caller's error says.
        void finish();
    };

} // namespace ddb

#endif // OUTPUT_FORMAT_H
