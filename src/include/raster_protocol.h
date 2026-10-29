/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_PROTOCOL_H
#define RASTER_PROTOCOL_H

#include <cstddef>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "basicgeometry.h"
#include "exif_protocol.h"
#include "fs.h"
#include "json.h"
#include "thumbs.h"

namespace ddb
{
    // The messages libddb and the raster helper (gdal-util, apps/gdal-util/PROTOCOL.md) exchange:
    // the lines each side sends and how it understands the lines that come back. No process, no
    // pipe here, and no GDAL: a line is plain data (a path, numbers, a WKT text), so a host
    // rebuilds what the helper found without opening a file.
    namespace raster
    {
        // The version this side implements. A helper is accepted when its major version is the
        // same and its minor version is not lower.
        constexpr int kMajorVersion = 1;
        constexpr int kMinorVersion = 0;

        // The longest line of either side
        constexpr size_t kMaxLineBytes = 16 * 1024 * 1024;

        // The `--version` line ends with "protocol <major>.<minor>", as the line of the other
        // helper programs does
        using Version = exif::protocol::Version;

        std::optional<Version> parseVersionOutput(std::string_view output);
        bool isAccepted(const Version &helper);
        std::string toString(const Version &version);

        // A line that is not what the protocol says (invalid JSON, a member missing or of the
        // wrong type, a base64 text that is none): a broken peer, or a malformed request
        class ProtocolError : public std::runtime_error
        {
          public:
            using std::runtime_error::runtime_error;
        };

        // What a failed request tells about the failure; the host maps each to what it does
        enum class ErrorCode
        {
            Unreadable, // the file cannot be opened or read as what it should be
            Policy,     // the open policy refuses the file
            Limit,      // a resource limit (memory, cpu time, size) was reached
            Crashed,    // the worker ended without an answer
            Timeout,    // the host's deadline passed
            NoSpace,    // no space left on the device the output goes to
            Internal    // anything else: a bad request, an operation that does not exist, a defect
        };

        const char *toString(ErrorCode code);
        std::optional<ErrorCode> parseErrorCode(std::string_view name);

        // The operations (the `op` member of a request)
        namespace op
        {
            constexpr const char *kProbe = "probe";
            constexpr const char *kThumbnail = "thumbnail";
            constexpr const char *kNormalize = "normalize";
            constexpr const char *kGeoproject = "geoproject";
            constexpr const char *kCog = "cog";
            constexpr const char *kVector = "vector";
        } // namespace op

        // A request. `path` is the file to read, `out` the file to write (operations that
        // write). `params` is an object whose members the operation defines.
        struct Request
        {
            int64_t id = 0;
            std::string op;
            fs::path path;
            fs::path out;
            json params = json::object();
        };

        // The request line (without the line feed). A path that is not valid UTF-8 goes out as
        // base64 in its own member (path64, out64); a relative path as it is: the sender makes
        // paths absolute.
        std::string encodeRequest(const Request &request);

        // @throws ProtocolError for a line that is not a request: not a JSON object, no integer
        // `id`, no string `op`, a malformed `path`/`out` member, `params` that is no object
        Request decodeRequest(std::string_view line);

        // The id of a request line that decodeRequest refuses, when it has one, so the answer
        // names it
        std::optional<int64_t> requestIdOf(std::string_view line);

        // A response. `result` is the operation's, when ok. The error and message otherwise.
        struct Response
        {
            // Empty when the request had none that could be read
            std::optional<int64_t> id;
            bool ok = false;
            json result = json::object();
            ErrorCode error = ErrorCode::Internal;
            std::string message;

            static Response success(std::optional<int64_t> id, json result);
            static Response failure(std::optional<int64_t> id, ErrorCode error, std::string message);
        };

        // The longest message of a failure, in bytes
        constexpr size_t kMaxMessageBytes = 2000;

        // The response line (without the line feed). A message that is not valid UTF-8 has its
        // bad bytes replaced and one that is longer than kMaxMessageBytes is cut.
        std::string encodeResponse(const Response &response);

        // @throws ProtocolError
        Response decodeResponse(std::string_view line);

        // ---- probe

        enum class ProbeKind
        {
            Raster,
            Vector
        };

        const char *toString(ProbeKind kind);

        // The params of a probe. For a vector, `source` is the file the user named when `path`
        // is a conversion of it (a DWG converted to a DXF): its sidecars give the coordinate
        // system. `converted` says it is one. Both stay empty for any other file.
        //
        // The .prj next to `source` gives a drawing its coordinate system, unless the sender
        // ignores it (the index re-reads a drawing right after its .prj was removed from the
        // index, while the file is still on disk: cad.h IgnoredSidecarScope). That rule lives in
        // the sender's thread, which the receiver cannot see, so a sender that applies it says
        // what it decided: `sidecarDecided`, and in `sidecar` the .prj that counts (empty: none
        // does). A receiver that is not told reads the .prj it finds.
        struct ProbeParams
        {
            ProbeKind kind = ProbeKind::Raster;
            fs::path source;
            bool converted = false;
            bool sidecarDecided = false;
            fs::path sidecar;
        };

        json toJson(const ProbeParams &params);
        // @throws ProtocolError
        ProbeParams probeParamsFromJson(const json &params);

        // What a probe found, as the entry of the file needs it
        struct ProbeResult
        {
            // "GeoRaster" (a raster with a projection), "Raster" (without) or "Vector"
            std::string type;
            // A GeoRaster that needs no conversion into a Cloud Optimized GeoTIFF
            bool isCog = false;
            // The members the entry adds to its properties (width, height, geotransform, bands;
            // vector, georeferenced): empty for a raster without a projection
            json properties = json::object();
            // The projection of a GeoRaster, WKT; empty otherwise. The entry keeps it as its
            // `projection` property.
            std::string wkt;
            BasicPointGeometry pointGeom;
            BasicPolygonGeometry polygonGeom;
            // What the entry shows as its warning when the probe could only describe the file in
            // part (a CAD drawing whose coordinate system GDAL cannot use keeps its properties
            // but has no footprint); empty when the probe is whole. At most kMaxMessageBytes.
            std::string warning;
        };

        json toJson(const ProbeResult &result);
        // @throws ProtocolError
        ProbeResult probeResultFromJson(const json &result);

        // ---- thumbnail

        // The params of a thumbnail: its size in pixels (the longest side) and, for the
        // rendering the visualisation parameters choose (bands, formula, colormap...), those
        // parameters. Without them the thumbnail is the plain one of the file.
        struct ThumbnailParams
        {
            int size = 0;
            std::optional<ThumbVisParams> vis;
            // What the tags of the file say (hasThermalTags, thermal.h): whether a single-band
            // raster of no known sensor renders a formula with its band as the thermal one. The
            // helper reads no tags, so the host reads them and says it here; needsThermalTags
            // says when.
            bool thermalTags = false;
        };

        // Whether the rendering of these params asks whether the file is thermal: a formula whose
        // band filter is detected from the file
        bool needsThermalTags(const ThumbnailParams &params);

        json toJson(const ThumbnailParams &params);
        // @throws ProtocolError
        ThumbnailParams thumbnailParamsFromJson(const json &params);

        // ---- builds

        // Receives the fraction (0 to 1) of a long operation that is done
        using ProgressCallback = std::function<void(double)>;

        // The line a one-shot process writes before its response while it works:
        // {"progress":<fraction>}, the fraction clamped to 0..1 (without the line feed)
        std::string encodeProgress(double fraction);

        // The fraction of a progress line; nullopt for any other line
        std::optional<double> decodeProgress(std::string_view line);

        // The params of a `cog`. The warp and the compression of the COG use the threads of one
        // build; the host holds the tokens of the CPU pool for them (parallel.h) and says how
        // many it got, since the helper has a pool of its own and could not know. Without
        // `threads` the build takes what the configuration (DDB_BUILD_THREADS) allows.
        struct CogParams
        {
            std::optional<int> threads;
        };

        json toJson(const CogParams &params);
        // @throws ProtocolError
        CogParams cogParamsFromJson(const json &params);

        // The params of a `vector` build. `source` is the file the user named when `path` is a
        // conversion of it (a DWG converted to a DXF by the host): its sidecars give the drawing
        // its coordinate system. Empty for any other file.
        struct VectorParams
        {
            fs::path source;
        };

        json toJson(const VectorParams &params);
        // @throws ProtocolError
        VectorParams vectorParamsFromJson(const json &params);

        // The params of a `geoproject`: what the host knows of a photo from its EXIF, since the
        // helper reads none. `footprint` is the ground position of the corners of the picture
        // (upper left, lower left, lower right, upper right) as longitude and latitude in
        // EPSG:4326, `width` and `height` its size in pixels, `outsize` the size of the result
        // as gdal_translate -outsize takes it ("100%" or a number of pixels of width; empty: as
        // the picture).
        struct GeoprojectParams
        {
            int width = 0;
            int height = 0;
            std::array<std::array<double, 2>, 4> footprint{};
            std::string outsize;
        };

        json toJson(const GeoprojectParams &params);
        // @throws ProtocolError
        GeoprojectParams geoprojectParamsFromJson(const json &params);

    } // namespace raster
} // namespace ddb

#endif // RASTER_PROTOCOL_H
