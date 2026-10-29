/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_protocol.h"

#include <cmath>
#include <utility>

#include "../../vendor/base64/base64.h"
#include "mio.h"

namespace ddb
{
    namespace raster
    {
        namespace
        {
            constexpr const char *kTypeGeoRaster = "GeoRaster";
            constexpr const char *kTypeRaster = "Raster";
            constexpr const char *kTypeVector = "Vector";

            // Whether the bytes are text a JSON string can hold: nlohmann's own check, the one
            // that decides whether dump() accepts them
            bool isJsonText(const std::string &text)
            {
                try
                {
                    (void)json(text).dump();
                    return true;
                }
                catch (const json::type_error &)
                {
                    return false;
                }
            }

            // Sets `name` to the text when it is UTF-8, else `name`64 to its base64: file names
            // are bytes, not always text
            void setText(json &object, const std::string &name, const std::string &text)
            {
                if (isJsonText(text))
                    object[name] = text;
                else
                    object[name + "64"] = Base64::encode(text);
            }

            const json *member(const json &object, const char *name)
            {
                const auto it = object.find(name);
                return it == object.end() || it->is_null() ? nullptr : &*it;
            }

            // The text of `name` or `name`64 (either, never both); nullopt when neither is there
            std::optional<std::string> getText(const json &object, const std::string &name)
            {
                const json *plain = member(object, name.c_str());
                const json *encoded = member(object, (name + "64").c_str());
                if (plain && encoded)
                    throw ProtocolError("both " + name + " and " + name + "64 are given");
                if (plain)
                {
                    if (!plain->is_string())
                        throw ProtocolError("'" + name + "' is not a string");
                    return plain->get<std::string>();
                }
                if (encoded)
                {
                    if (!encoded->is_string())
                        throw ProtocolError("'" + name + "64' is not a string");
                    const std::string text = encoded->get<std::string>();
                    // Only the canonical form (padded, in the alphabet) is base64 here: decoding and
                    // encoding again gives the text back exactly then
                    const std::string bytes = Base64::decode(text);
                    if (Base64::encode(bytes) != text)
                        throw ProtocolError("'" + name + "64' is not base64");
                    return bytes;
                }
                return std::nullopt;
            }

            fs::path getPath(const json &object, const std::string &name)
            {
                const auto text = getText(object, name);
                return text ? io::fromUtf8(*text) : fs::path();
            }

            json parseObject(std::string_view line, const char *what)
            {
                json parsed = json::parse(line.begin(), line.end(), nullptr, false);
                if (parsed.is_discarded())
                    throw ProtocolError(std::string("not a valid JSON line: ") + what);
                if (!parsed.is_object())
                    throw ProtocolError(std::string(what) + " is not a JSON object");
                return parsed;
            }

            std::optional<int64_t> readId(const json &object)
            {
                const json *id = member(object, "id");
                if (!id)
                    return std::nullopt;
                if (!id->is_number_integer())
                    throw ProtocolError("'id' is not an integer");
                return id->get<int64_t>();
            }

            const json &required(const json &object, const char *name)
            {
                const json *value = member(object, name);
                if (!value)
                    throw ProtocolError(std::string("'") + name + "' is missing");
                return *value;
            }

            std::string requiredString(const json &object, const char *name)
            {
                const json &value = required(object, name);
                if (!value.is_string())
                    throw ProtocolError(std::string("'") + name + "' is not a string");
                return value.get<std::string>();
            }

            // The longest prefix of `text` that is at most `limit` bytes and does not end inside
            // a UTF-8 sequence
            std::string cutAtCharacter(const std::string &text, size_t limit)
            {
                if (text.size() <= limit)
                    return text;
                size_t end = limit;
                while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
                    end--;
                return text.substr(0, end);
            }

            // ---- geometry

            json coordinateOf(const Point &p)
            {
                return json::array({p.x, p.y, p.z});
            }

            Point pointFromJson(const json &coordinate)
            {
                if (!coordinate.is_array() || (coordinate.size() != 2 && coordinate.size() != 3))
                    throw ProtocolError("a coordinate is not an array of 2 or 3 numbers");
                for (const auto &n : coordinate)
                    if (!n.is_number())
                        throw ProtocolError("a coordinate holds a value that is not a number");
                return Point(coordinate[0].get<double>(), coordinate[1].get<double>(),
                             coordinate.size() == 3 ? coordinate[2].get<double>() : 0.0);
            }

            json pointGeometryJson(const BasicPointGeometry &geometry)
            {
                return {{"type", "Point"}, {"coordinates", coordinateOf(geometry.points.front())}};
            }

            json polygonGeometryJson(const BasicPolygonGeometry &geometry)
            {
                json ring = json::array();
                for (const auto &p : geometry.points)
                    ring.push_back(coordinateOf(p));
                return {{"type", "Polygon"}, {"coordinates", json::array({ring})}};
            }

            const json &coordinatesOf(const json &geometry, const char *type)
            {
                if (!geometry.is_object() || requiredString(geometry, "type") != type)
                    throw ProtocolError(std::string("a geometry is not a ") + type);
                const json &coordinates = required(geometry, "coordinates");
                if (!coordinates.is_array())
                    throw ProtocolError("'coordinates' is not an array");
                return coordinates;
            }

            BasicPointGeometry pointGeometryFromJson(const json &geometry)
            {
                BasicPointGeometry result;
                const Point p = pointFromJson(coordinatesOf(geometry, "Point"));
                result.addPoint(p.x, p.y, p.z);
                return result;
            }

            BasicPolygonGeometry polygonGeometryFromJson(const json &geometry)
            {
                BasicPolygonGeometry result;
                const json &rings = coordinatesOf(geometry, "Polygon");
                if (rings.size() != 1 || !rings[0].is_array())
                    throw ProtocolError("a Polygon holds one ring");
                for (const auto &coordinate : rings[0])
                {
                    const Point p = pointFromJson(coordinate);
                    result.addPoint(p.x, p.y, p.z);
                }
                return result;
            }
        } // namespace

        std::optional<Version> parseVersionOutput(std::string_view output)
        {
            return exif::protocol::parseVersionOutput(output);
        }

        bool isAccepted(const Version &helper)
        {
            return helper.major == kMajorVersion && helper.minor >= kMinorVersion;
        }

        std::string toString(const Version &version)
        {
            return exif::protocol::toString(version);
        }

        const char *toString(ErrorCode code)
        {
            switch (code)
            {
            case ErrorCode::Unreadable:
                return "unreadable";
            case ErrorCode::Policy:
                return "policy";
            case ErrorCode::Limit:
                return "limit";
            case ErrorCode::Crashed:
                return "crashed";
            case ErrorCode::Timeout:
                return "timeout";
            case ErrorCode::NoSpace:
                return "nospace";
            case ErrorCode::Internal:
                return "internal";
            }
            return "internal";
        }

        std::optional<ErrorCode> parseErrorCode(std::string_view name)
        {
            for (const ErrorCode code : {ErrorCode::Unreadable, ErrorCode::Policy, ErrorCode::Limit, ErrorCode::Crashed,
                                         ErrorCode::Timeout, ErrorCode::NoSpace, ErrorCode::Internal})
            {
                if (name == toString(code))
                    return code;
            }
            return std::nullopt;
        }

        const char *toString(ProbeKind kind)
        {
            return kind == ProbeKind::Vector ? "vector" : "raster";
        }

        // ---- requests and responses

        std::string encodeRequest(const Request &request)
        {
            json line = {{"id", request.id}, {"op", request.op}};
            setText(line, "path", io::toUtf8(request.path));
            if (!request.out.empty())
                setText(line, "out", io::toUtf8(request.out));
            line["params"] = request.params;
            return line.dump();
        }

        std::optional<int64_t> requestIdOf(std::string_view line)
        {
            const json parsed = json::parse(line.begin(), line.end(), nullptr, false);
            if (!parsed.is_object())
                return std::nullopt;
            const json *id = member(parsed, "id");
            if (!id || !id->is_number_integer())
                return std::nullopt;
            return id->get<int64_t>();
        }

        Request decodeRequest(std::string_view line)
        {
            const json parsed = parseObject(line, "the request");
            Request request;
            const auto id = readId(parsed);
            if (!id)
                throw ProtocolError("'id' is missing");
            request.id = *id;
            request.op = requiredString(parsed, "op");
            request.path = getPath(parsed, "path");
            request.out = getPath(parsed, "out");
            if (const json *params = member(parsed, "params"))
            {
                if (!params->is_object())
                    throw ProtocolError("'params' is not an object");
                request.params = *params;
            }
            return request;
        }

        Response Response::success(std::optional<int64_t> id, json result)
        {
            Response response;
            response.id = id;
            response.ok = true;
            response.result = std::move(result);
            return response;
        }

        Response Response::failure(std::optional<int64_t> id, ErrorCode error, std::string message)
        {
            Response response;
            response.id = id;
            response.ok = false;
            response.error = error;
            response.message = std::move(message);
            return response;
        }

        std::string encodeResponse(const Response &response)
        {
            json line = {{"id", response.id ? json(*response.id) : json(nullptr)}, {"ok", response.ok}};
            if (response.ok)
            {
                line["result"] = response.result;
            }
            else
            {
                line["error"] = toString(response.error);
                line["message"] = cutAtCharacter(response.message, kMaxMessageBytes);
            }
            // A message holds bytes of GDAL's or the system's choosing: whatever is not UTF-8 in it is
            // replaced instead of failing the whole response
            return line.dump(-1, ' ', false, json::error_handler_t::replace);
        }

        Response decodeResponse(std::string_view line)
        {
            const json parsed = parseObject(line, "the response");
            Response response;
            response.id = readId(parsed);
            const json &ok = required(parsed, "ok");
            if (!ok.is_boolean())
                throw ProtocolError("'ok' is not a boolean");
            response.ok = ok.get<bool>();
            if (response.ok)
            {
                if (const json *result = member(parsed, "result"))
                {
                    if (!result->is_object())
                        throw ProtocolError("'result' is not an object");
                    response.result = *result;
                }
                return response;
            }

            const std::string error = requiredString(parsed, "error");
            const auto code = parseErrorCode(error);
            if (!code)
                throw ProtocolError("unknown error code '" + error + "'");
            response.error = *code;
            if (const json *message = member(parsed, "message"))
            {
                if (!message->is_string())
                    throw ProtocolError("'message' is not a string");
                response.message = message->get<std::string>();
            }
            return response;
        }

        // ---- probe

        json toJson(const ProbeParams &params)
        {
            json result = {{"kind", toString(params.kind)}};
            if (!params.source.empty())
                setText(result, "source", io::toUtf8(params.source));
            if (params.converted)
                result["converted"] = true;
            return result;
        }

        ProbeParams probeParamsFromJson(const json &params)
        {
            if (!params.is_object())
                throw ProtocolError("the probe params are not an object");
            ProbeParams result;
            const std::string kind = requiredString(params, "kind");
            if (kind == toString(ProbeKind::Raster))
                result.kind = ProbeKind::Raster;
            else if (kind == toString(ProbeKind::Vector))
                result.kind = ProbeKind::Vector;
            else
                throw ProtocolError("unknown probe kind '" + kind + "'");
            result.source = getPath(params, "source");
            if (const json *converted = member(params, "converted"))
            {
                if (!converted->is_boolean())
                    throw ProtocolError("'converted' is not a boolean");
                result.converted = converted->get<bool>();
            }
            return result;
        }

        json toJson(const ProbeResult &result)
        {
            json out = {{"type", result.type}, {"isCog", result.isCog}, {"properties", result.properties}};
            if (!result.wkt.empty())
                out["wkt"] = result.wkt;
            if (!result.pointGeom.empty())
                out["pointGeom"] = pointGeometryJson(result.pointGeom);
            if (!result.polygonGeom.empty())
                out["polygonGeom"] = polygonGeometryJson(result.polygonGeom);
            return out;
        }

        ProbeResult probeResultFromJson(const json &result)
        {
            if (!result.is_object())
                throw ProtocolError("the probe result is not an object");
            ProbeResult out;
            out.type = requiredString(result, "type");
            if (out.type != kTypeGeoRaster && out.type != kTypeRaster && out.type != kTypeVector)
                throw ProtocolError("unknown probe type '" + out.type + "'");

            if (const json *isCog = member(result, "isCog"))
            {
                if (!isCog->is_boolean())
                    throw ProtocolError("'isCog' is not a boolean");
                out.isCog = isCog->get<bool>();
            }
            const json &properties = required(result, "properties");
            if (!properties.is_object())
                throw ProtocolError("'properties' is not an object");
            out.properties = properties;

            if (const json *wkt = member(result, "wkt"))
            {
                if (!wkt->is_string())
                    throw ProtocolError("'wkt' is not a string");
                out.wkt = wkt->get<std::string>();
            }
            if (const json *point = member(result, "pointGeom"))
                out.pointGeom = pointGeometryFromJson(*point);
            if (const json *polygon = member(result, "polygonGeom"))
                out.polygonGeom = polygonGeometryFromJson(*polygon);
            return out;
        }

        // ---- thumbnail

        namespace
        {
            // The visualisation parameters by their member name, to read and write them with one list
            struct VisMember
            {
                const char *name;
                std::string ThumbVisParams::*field;
            };

            constexpr VisMember kVisMembers[] = {
                {"preset", &ThumbVisParams::preset},         {"bands", &ThumbVisParams::bands},
                {"formula", &ThumbVisParams::formula},       {"bandFilter", &ThumbVisParams::bandFilter},
                {"colormap", &ThumbVisParams::colormap},     {"rescale", &ThumbVisParams::rescale}};
        } // namespace

        json toJson(const ThumbnailParams &params)
        {
            json result = {{"size", params.size}};
            if (params.vis)
            {
                json vis = json::object();
                for (const auto &m : kVisMembers)
                {
                    const std::string &value = (*params.vis).*(m.field);
                    if (!value.empty())
                        vis[m.name] = value;
                }
                result["vis"] = vis;
            }
            return result;
        }

        ThumbnailParams thumbnailParamsFromJson(const json &params)
        {
            if (!params.is_object())
                throw ProtocolError("the thumbnail params are not an object");
            ThumbnailParams result;
            const json &size = required(params, "size");
            if (!size.is_number_integer())
                throw ProtocolError("'size' is not an integer");
            result.size = size.get<int>();
            if (const json *vis = member(params, "vis"))
            {
                if (!vis->is_object())
                    throw ProtocolError("'vis' is not an object");
                ThumbVisParams parsed;
                for (const auto &m : kVisMembers)
                {
                    if (const json *value = member(*vis, m.name))
                    {
                        if (!value->is_string())
                            throw ProtocolError(std::string("'") + m.name + "' is not a string");
                        parsed.*(m.field) = value->get<std::string>();
                    }
                }
                result.vis = parsed;
            }
            return result;
        }

    } // namespace raster
} // namespace ddb
