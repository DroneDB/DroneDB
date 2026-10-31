/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "output_format.h"

#include <algorithm>
#include <ostream>

#include "exceptions.h"
#include "json.h"

namespace ddb
{

    const char *outputFormatName(OutputFormat format) noexcept
    {
        switch (format)
        {
        case OutputFormat::Text:
            return "text";
        case OutputFormat::Json:
            return "json";
        case OutputFormat::GeoJson:
            return "geojson";
        }
        return "text";
    }

    std::optional<OutputFormat> findOutputFormat(const std::string &name)
    {
        for (const OutputFormat format : {OutputFormat::Text, OutputFormat::Json, OutputFormat::GeoJson})
            if (name == outputFormatName(format))
                return format;
        return std::nullopt;
    }

    OutputFormat parseOutputFormat(const std::string &name, std::initializer_list<OutputFormat> allowed)
    {
        const auto format = findOutputFormat(name);
        if (!format || std::find(allowed.begin(), allowed.end(), *format) == allowed.end())
            throw InvalidArgsException("Invalid format " + name);
        return *format;
    }

    void requireOutputFormat(OutputFormat format, std::initializer_list<OutputFormat> allowed)
    {
        if (std::find(allowed.begin(), allowed.end(), format) == allowed.end())
            throw InvalidArgsException(std::string("Invalid format ") + outputFormatName(format));
    }

    JsonArrayWriter::JsonArrayWriter(std::ostream &out, const std::string &open, std::string close)
        : out_(out), close_(std::move(close))
    {
        out_ << open;
    }

    void JsonArrayWriter::write(const json &member)
    {
        if (!first_)
            out_ << ",";
        out_ << member.dump();
        first_ = false;
    }

    void JsonArrayWriter::finish()
    {
        out_ << close_;
    }

} // namespace ddb
