/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gdal_open_policy.h"

#include <cpl_conv.h>
#include <cpl_http.h>
#include <cpl_minixml.h>
#include <cpl_vsi.h>

#include <algorithm>
#include <initializer_list>
#include <string_view>

#include "logger.h"
#include "mio.h"
#include "stringutil.h"

namespace ddb
{
    namespace
    {
        // Some drivers request URLs the file names while they open it: the GML driver fetches
        // the schemas its .xsd includes, whatever the configuration, and the targets of its
        // xlinks when the host turns their resolution on. A file of a user names any URL it
        // likes, so while one opens, every HTTP request GDAL makes on this thread fails
        // (the callbacks are per thread). The network file systems of GDAL (/vsicurl/,
        // /vsis3/, ...) do not request through those callbacks but through curl itself, and
        // a schema the .xsd includes may be a /vsicurl/ path: while a file opens, they open
        // no other file than the one being opened (a remote raster libddb opens by its URL),
        // a configuration option of this thread alone.
        class NoHttpWhileOpening
        {
          public:
            explicit NoHttpWhileOpening(const std::string &path)
                : pushed_(CPLHTTPPushFetchCallback(&refuse, nullptr) != FALSE),
                  onlyThisFile_("CPL_VSIL_CURL_ALLOWED_FILENAME", path.c_str())
            {
            }
            ~NoHttpWhileOpening()
            {
                if (pushed_)
                    CPLHTTPPopFetchCallback();
            }
            NoHttpWhileOpening(const NoHttpWhileOpening &) = delete;
            NoHttpWhileOpening &operator=(const NoHttpWhileOpening &) = delete;

          private:
            static CPLHTTPResult *refuse(const char *url, CSLConstList, GDALProgressFunc, void *,
                                         CPLHTTPFetchWriteFunc, void *, void *)
            {
                LOGW << "Refused to request " << url << " while opening a file";
                auto *result = static_cast<CPLHTTPResult *>(CPLCalloc(1, sizeof(CPLHTTPResult)));
                result->nStatus = 1;
                result->pszErrBuf = CPLStrdup("no request is made while a file of a user opens");
                return result;
            }

            bool pushed_;
            ScopedThreadConfigOption onlyThisFile_;
        };

        GDALDatasetUniquePtr openWith(const std::string &path, const std::vector<std::string> &drivers,
                                      unsigned int kind)
        {
            if (drivers.empty())
            {
                LOGD << "Not opening " << path << ": its extension is not one that is read as such";
                return nullptr;
            }
            const CPLStringList allowed = cslFromStrings(drivers);
            const NoHttpWhileOpening noHttp(path);
            return adoptDataset(GDALOpenEx(path.c_str(), kind | GDAL_OF_READONLY | GDAL_OF_VERBOSE_ERROR,
                                           allowed.List(), nullptr, nullptr));
        }
    } // namespace

    namespace gdalopen
    {
        namespace
        {
            // The drivers a file with an extension may be opened with, the one table of
            // extensions both the raster and the vector opens read. The extensions are the ones
            // the entry handlers index (image, georaster, vector) and the ones the tools read
            // from a dataset, a driver per format of the extension, none of them a driver that
            // reads other files than the one it is given (VRT, OGR_VRT, ...).
            struct ExtensionDrivers
            {
                std::string_view extension;
                std::vector<std::string_view> drivers;
                bool vrt = false;
            };

            const std::vector<ExtensionDrivers> &rasterTable()
            {
                static const std::vector<ExtensionDrivers> table = {
                    {"tif", {"GTiff", "COG", "LIBERTIFF"}},
                    {"tiff", {"GTiff", "COG", "LIBERTIFF"}},
                    // A GeoTIFF named for what it is
                    {"cog", {"GTiff", "COG", "LIBERTIFF"}},
                    {"jp2", {"JP2OpenJPEG", "JPEG2000", "JP2KAK", "JP2ECW", "JP2MrSID", "JP2Lura"}},
                    {"j2k", {"JP2OpenJPEG", "JPEG2000", "JP2KAK", "JP2ECW", "JP2MrSID", "JP2Lura"}},
                    {"jpg", {"JPEG"}},
                    {"jpeg", {"JPEG"}},
                    {"png", {"PNG"}},
                    {"gif", {"GIF"}},
                    {"webp", {"WEBP"}},
                    // A DNG is a TIFF
                    {"dng", {"GTiff", "LIBERTIFF"}},
                    // The only extension whose file lists other files to read: validateVrt
                    // checks it before it opens
                    {"vrt", {"VRT"}, true},
                };
                return table;
            }

            const std::vector<ExtensionDrivers> &vectorTable()
            {
                static const std::vector<ExtensionDrivers> table = {
                    {"geojson", {"GeoJSON", "GeoJSONSeq", "ESRIJSON", "TopoJSON"}},
                    {"topojson", {"TopoJSON", "GeoJSON"}},
                    {"dxf", {"DXF"}},
                    {"shp", {"ESRI Shapefile"}},
                    {"shz", {"ESRI Shapefile"}},
                    {"fgb", {"FlatGeobuf"}},
                    {"kml", {"KML", "LIBKML"}},
                    {"kmz", {"LIBKML"}},
                    {"gpkg", {"GPKG"}},
                    {"gpx", {"GPX"}},
                    // Not indexed, but built and read through the tools
                    {"gml", {"GML"}},
                };
                return table;
            }

            const ExtensionDrivers *find(const std::vector<ExtensionDrivers> &table, const std::string &path)
            {
                const std::string ext = io::lowerExtension(io::fromUtf8(path));
                const auto it = std::find_if(table.begin(), table.end(),
                                             [&ext](const ExtensionDrivers &e) { return e.extension == ext; });
                return it == table.end() ? nullptr : &*it;
            }

            // The drivers of an entry the linked GDAL has (a build may lack LIBERTIFF, JP2KAK...)
            std::vector<std::string> presentDrivers(const ExtensionDrivers *entry)
            {
                std::vector<std::string> present;
                if (entry == nullptr)
                    return present;
                for (const auto name : entry->drivers)
                {
                    const std::string driver(name);
                    if (GDALGetDriverByName(driver.c_str()) != nullptr)
                        present.push_back(driver);
                }
                return present;
            }

            // A VRT may name VRTs that name VRTs; beyond this many levels it is refused
            constexpr int MaxVrtNesting = 2;

            VrtVerdict refuse(const std::string &reason)
            {
                return {false, reason};
            }

            // The metadata items GDAL reads as the name of another dataset, which it opens with
            // all its drivers wherever it is (any path, a /vsicurl/ URL, a VRT): the overviews
            // of a raster that has no .ovr or .aux next to it (OVERVIEW_FILE, read the first
            // time its overviews are asked for, gcore/gdaldefaultoverviews.cpp) and the
            // geolocation arrays a warp reads when it has no geotransform (alg/gdalgeoloc.cpp).
            // A TIFF carries any metadata domain in its GDAL_METADATA tag and a JPEG 2000 in its
            // GDAL metadata box, and GDAL has no option to ignore them. No file of a format
            // libddb reads needs one: GDAL itself writes OVERVIEW_FILE only for the subdatasets
            // of formats libddb does not read, and geolocation arrays for netCDF and HDF.
            struct MetadataFile
            {
                const char *domain;
                const char *item;
            };
            constexpr MetadataFile MetadataFiles[] = {
                {"OVERVIEWS", "OVERVIEW_FILE"},
                {"GEOLOCATION", "X_DATASET"},
                {"GEOLOCATION", "Y_DATASET"},
            };

            // Whether the opened dataset names no other dataset in its metadata
            VrtVerdict validateMetadataFiles(GDALDataset &dataset)
            {
                for (const auto &[domain, item] : MetadataFiles)
                    if (const char *file = dataset.GetMetadataItem(item, domain); file != nullptr)
                        return refuse(std::string("its ") + domain + " metadata names the file " + file);
                return {true, {}};
            }

            // Opens a file as a user file is opened, with `drivers` alone, and checks that its
            // metadata names no other dataset; refused when it does not open or names one
            VrtVerdict validateOpened(const std::string &file, const std::vector<std::string> &drivers,
                                      unsigned int kind)
            {
                const auto dataset = openWith(file, drivers, kind);
                if (!dataset)
                    return refuse(file + " does not open as a file of its kind");
                if (const auto v = validateMetadataFiles(*dataset); !v.ok)
                    return refuse(file + ": " + v.reason);
                return {true, {}};
            }

            bool hasDriveOrRoot(const fs::path &p)
            {
                return p.is_absolute() || p.has_root_name() || p.has_root_directory() ||
                       (!p.empty() && (*p.native().begin() == '/' || *p.native().begin() == '\\'));
            }

            // Whether GDAL, opening `file` with all its drivers of `kind` (raster or vector) as
            // it opens a sidecar, opens it with `driver`. Identified with that driver alone
            // first: once it claims the file, the identification with all drivers stops at the
            // first that claims it, never falling back to opening the file with each of them.
            bool sidecarOpensAs(const std::string &file, const char *driver, unsigned int kind)
            {
                const char *const only[] = {driver, nullptr};
                if (GDALIdentifyDriverEx(file.c_str(), kind, only, nullptr) == nullptr)
                    return false;
                const GDALDriverH first = GDALIdentifyDriverEx(file.c_str(), kind, nullptr, nullptr);
                return first != nullptr && str::iequals(GDALGetDriverShortName(first), driver);
            }

            bool exists(const std::string &file)
            {
                VSIStatBufL stat;
                return VSIStatExL(file.c_str(), &stat, VSI_STAT_EXISTS_FLAG) == 0;
            }

            // An ERDAS .aux is opened by GDAL only when it starts with this tag
            bool hasHfaHeader(const std::string &file)
            {
                VSILFILE *fp = VSIFOpenL(file.c_str(), "rb");
                if (fp == nullptr)
                    return false;
                char header[32] = {};
                const bool read = VSIFReadL(header, 1, sizeof(header), fp) == sizeof(header);
                CPL_IGNORE_RET_VAL(VSIFCloseL(fp));
                return read && str::startsWithCI(std::string(header, sizeof(header)), "EHFA_HEADER_TAG");
            }

            // GDAL looks for no sidecar of these (a network path with a query, a part of a file)
            bool takesSidecars(const std::string &path)
            {
                return !(path.find("/vsicurl/") != std::string::npos && path.find('?') != std::string::npos) &&
                       path.rfind("/vsisubfile/", 0) != 0;
            }

            // A sidecar of a sidecar of ... beyond this many levels is refused
            constexpr int MaxSidecarNesting = 4;

            // The files next to a raster that GDAL opens with ALL its drivers, whatever the
            // drivers the raster itself was opened with (gcore/gdaldefaultoverviews.cpp,
            // GDALFindAssociatedAuxFile, the attribute table of a GeoTIFF band): the external
            // overviews <name>.ovr, the external mask <name>.msk (each also upper case, on a file
            // system that tells them apart), the ERDAS .aux (<name>.aux, or the name with its
            // extension replaced) that holds overviews and the ESRI attribute table
            // <name>.vat.dbf, opened with all the vector drivers. GDAL writes the first two as
            // GeoTIFF, the third as HFA and the fourth is a dBase table; anything else under such
            // a name, a VRT above all, would read the files and URLs it lists through a raster
            // whose own drivers read nothing else. Each is opened as what it must be and checked
            // for the datasets its metadata names, as the raster is. GDAL looks for the sidecars
            // of a raster sidecar in turn (the overviews of the overviews, ...), and so does the
            // check.
            VrtVerdict validateSidecars(const std::string &path, int depth)
            {
                if (!takesSidecars(path))
                    return {true, {}};

                struct Sidecar
                {
                    std::string file;
                    const char *driver;
                    unsigned int kind;
                };
                std::vector<Sidecar> sidecars;
                for (const char *suffix : {".ovr", ".OVR", ".msk", ".MSK"})
                    if (const std::string f = path + suffix; exists(f))
                        sidecars.push_back({f, "GTiff", GDAL_OF_RASTER});
                for (const std::string &f : {CPLResetExtensionSafe(path.c_str(), "aux"),
                                             CPLResetExtensionSafe(path.c_str(), "AUX"), path + ".aux", path + ".AUX"})
                    if (f != path && exists(f) && hasHfaHeader(f))
                        sidecars.push_back({f, "HFA", GDAL_OF_RASTER});
                if (const std::string f = path + ".vat.dbf"; exists(f))
                    sidecars.push_back({f, "ESRI Shapefile", GDAL_OF_VECTOR});

                for (const auto &sidecar : sidecars)
                {
                    if (depth + 1 > MaxSidecarNesting)
                        return refuse("sidecar " + sidecar.file + " is nested deeper than " +
                                      std::to_string(MaxSidecarNesting));
                    std::error_code ec;
                    if (fs::is_symlink(io::fromUtf8(sidecar.file), ec))
                        return refuse("sidecar " + sidecar.file + " is a symbolic link");
                    if (!sidecarOpensAs(sidecar.file, sidecar.driver, sidecar.kind))
                        return refuse("sidecar " + sidecar.file + " is not a " + sidecar.driver + " file");
                    if (sidecar.kind == GDAL_OF_RASTER)
                        if (const auto v = validateSidecars(sidecar.file, depth + 1); !v.ok)
                            return v;
                    if (const auto v = validateOpened(sidecar.file, {sidecar.driver}, sidecar.kind); !v.ok)
                        return refuse("sidecar " + v.reason);
                }
                return {true, {}};
            }

            VrtVerdict validateVrtAt(const fs::path &path, const fs::path &root, int depth);

            // A source the VRT at vrt names: relative, inside root, and a file its own
            // extension allows (a VRT is checked in turn)
            VrtVerdict validateSource(const std::string &source, const char *relativeToVrt, const fs::path &vrt,
                                      const fs::path &root, int depth)
            {
                if (source.empty())
                    return refuse("a source has no name");
                if (str::startsWithCI(source, "/vsi"))
                    return refuse("source " + source + " is a virtual file system path");
                // GDAL resolves only the path part of a name with a driver prefix
                // (GTIFF_DIR:1:<path>, NITF_IM:0:<path>, ...) against the folder of the VRT, so
                // the prefix reads as a folder here and a ".." after it climbs out unseen; a URL
                // and a drive have a ':' too. No relative file name of a dataset needs one.
                if (source.find(':') != std::string::npos)
                    return refuse("source " + source + " has a driver prefix, a drive or a URL");
                // A VRT written inline is a dataset of its own: its relative sources resolve
                // against the working folder and its nesting is not counted
                if (source.find('<') != std::string::npos)
                    return refuse("source " + source + " is an inline dataset");
                if (std::string_view(relativeToVrt) != "1")
                    return refuse("source " + source + " is not relative to the VRT");

                const fs::path sourcePath = io::fromUtf8(source);
                if (hasDriveOrRoot(sourcePath))
                    return refuse("source " + source + " is an absolute path");

                std::error_code ec;
                const fs::path resolved = fs::weakly_canonical(vrt.parent_path() / sourcePath, ec);
                const fs::path canonicalRoot = fs::weakly_canonical(root, ec);
                if (ec)
                    return refuse("source " + source + " cannot be resolved");
                const fs::path inside = resolved.lexically_relative(canonicalRoot);
                if (inside.empty() || inside == "." || *inside.begin() == "..")
                    return refuse("source " + source + " is outside " + io::toUtf8(root));

                const std::string resolvedUtf8 = io::toUtf8(resolved);
                if (rasterDrivers(resolvedUtf8).empty())
                    return refuse("source " + source + " is not a raster format that is read");
                if (identifyUserRaster(resolvedUtf8) == nullptr)
                    return refuse("source " + source + " is not a file of its extension");
                // The VRT opens the source by the name it gives, and GDAL looks for the
                // sidecars next to that name
                if (const auto v = validateSidecars(io::toUtf8(vrt.parent_path() / sourcePath), 0); !v.ok)
                    return v;

                if (isVrtPath(resolvedUtf8))
                {
                    if (depth + 1 > MaxVrtNesting)
                        return refuse("VRTs are nested deeper than " + std::to_string(MaxVrtNesting));
                    if (const auto v = validateVrtAt(resolved, root, depth + 1); !v.ok)
                        return v;
                }
                // The VRT reads the overviews of its sources, and GDAL those its metadata names.
                // Opened only now that, when it is a VRT, its own sources are known to be safe.
                if (const auto v = validateOpened(resolvedUtf8, rasterDrivers(resolvedUtf8), GDAL_OF_RASTER); !v.ok)
                    return refuse("source " + v.reason);
                return {true, {}};
            }

            // What a VRT of a user may hold, and nothing else: the elements and attributes GDAL's
            // VRT driver reads for a plain dataset of sourced or derived bands (its schema,
            // frmts/vrt/data/gdalvrt.xsd). Each is a value GDAL parses in place, apart from
            // SourceFilename, the one name of a file, which validateSource checks; it names the
            // sources a band reads and the files of the overviews of a band (Overview), both
            // opened with all the drivers of GDAL. Everything else GDAL knows is refused,
            // because it makes the open read a file or URL the source checks never see: a warped
            // dataset deserialises its warp options while it opens, and a geolocation
            // transformer opens the dataset its X_DATASET names, an RPC transformer the DEM its
            // DEMPath names, both any path or URL; pansharpened and processed datasets have
            // inputs of their own, raw bands read any file at an offset, a source may be an
            // inline VRT or come with open options, a multidimensional group and an array source
            // name arrays of other files, and a Python pixel function runs code. GDAL reads its
            // element and attribute names case-insensitively, and so are they compared here.
            struct VrtElement
            {
                std::string_view name;
                std::vector<std::string_view> attributes;
                std::vector<std::string_view> children;
                // Its attributes are free (the named arguments of a pixel function)
                bool anyAttribute = false;
            };

            std::vector<std::string_view> concat(std::initializer_list<std::vector<std::string_view>> lists)
            {
                std::vector<std::string_view> all;
                for (const auto &list : lists)
                    all.insert(all.end(), list.begin(), list.end());
                return all;
            }

            const std::vector<VrtElement> &vrtElements()
            {
                static const std::vector<VrtElement> elements = [] {
                    // A simple, averaged or complex source and the sources that extend them
                    const std::vector<std::string_view> sourceAttributes = {"resampling", "name"};
                    const std::vector<std::string_view> source = {"SourceFilename", "SourceBand", "SourceProperties",
                                                                  "SrcRect", "DstRect"};
                    const std::vector<std::string_view> complex = concat(
                        {source,
                         {"ScaleOffset", "ScaleRatio", "Exponent", "SrcMin", "SrcMax", "DstMin", "DstMax", "Clip",
                          "NODATA", "UseMaskBand", "LUT", "ColorTableComponent"}});
                    const std::vector<std::string_view> rect = {"xOff", "yOff", "xSize", "ySize"};

                    std::vector<VrtElement> all = {
                        {"VRTDataset",
                         {"rasterXSize", "rasterYSize"},
                         {"SRS", "GeoTransform", "GCPList", "Metadata", "MaskBand", "VRTRasterBand", "OverviewList"}},
                        // Read with the limitations of GDAL that forbid files and the network
                        {"SRS", {"dataAxisToSRSAxisMapping", "coordinateEpoch"}, {}},
                        {"GCPList", {"Projection", "dataAxisToSRSAxisMapping"}, {"GCP"}},
                        {"GCP", {"Id", "Info", "Pixel", "Line", "X", "Y", "Z", "GCPZ"}, {}},
                        // Not in the domains whose items GDAL opens (validateMetadataDomain)
                        {"Metadata", {"domain"}, {"MDI"}},
                        {"MDI", {"key"}, {}},
                        {"MaskBand", {}, {"VRTRasterBand"}},
                        // Overviews computed from the sources, at these factors
                        {"OverviewList", {"resampling"}, {}},
                        // Of a subClass validateBandClass accepts
                        {"VRTRasterBand",
                         {"dataType", "band", "blockXSize", "blockYSize", "subClass"},
                         {"Description", "NoDataValue", "HideNoDataValue", "UnitType", "Offset", "Scale",
                          "ColorInterp", "CategoryNames", "ColorTable", "Metadata", "Overview", "MaskBand",
                          "SimpleSource", "ComplexSource", "AveragedSource", "NoDataFromMaskSource",
                          "KernelFilteredSource", "PixelFunctionType", "PixelFunctionArguments",
                          "SourceTransferType", "SkipNonContributingSources"}},
                        {"CategoryNames", {}, {"Category"}},
                        {"ColorTable", {}, {"Entry"}},
                        {"Entry", {"c1", "c2", "c3", "c4"}, {}},
                        {"Overview", {}, {"SourceFilename", "SourceBand"}},
                        // A function compiled into GDAL; without PixelFunctionLanguage and
                        // PixelFunctionCode, none is Python
                        {"PixelFunctionArguments", {}, {}, true},
                        {"SimpleSource", sourceAttributes, source},
                        {"AveragedSource", sourceAttributes, source},
                        {"ComplexSource", sourceAttributes, complex},
                        {"NoDataFromMaskSource", sourceAttributes,
                         concat({source, {"NODATA", "MaskValueThreshold", "RemappedValue"}})},
                        {"KernelFilteredSource", sourceAttributes, concat({complex, {"Kernel", "Function"}})},
                        {"Kernel", {"normalized"}, {"Size", "Coefs"}},
                        {"SourceFilename", {"relativeToVRT", "shared"}, {}},
                        {"SourceProperties", {"RasterXSize", "RasterYSize", "DataType", "BlockXSize", "BlockYSize"},
                         {}},
                        {"SrcRect", rect, {}},
                        {"DstRect", rect, {}},
                    };
                    // The values: neither attributes nor elements of their own
                    for (const std::string_view value :
                         {"GeoTransform", "Category", "Description", "NoDataValue", "HideNoDataValue", "UnitType",
                          "Offset", "Scale", "ColorInterp", "PixelFunctionType", "SourceTransferType",
                          "SkipNonContributingSources", "SourceBand", "ScaleOffset", "ScaleRatio", "Exponent",
                          "SrcMin", "SrcMax", "DstMin", "DstMax", "Clip", "NODATA", "UseMaskBand", "LUT",
                          "ColorTableComponent", "MaskValueThreshold", "RemappedValue", "Function", "Size", "Coefs"})
                        all.push_back({value, {}, {}});
                    return all;
                }();
                return elements;
            }

            bool listed(const std::vector<std::string_view> &names, std::string_view name)
            {
                return std::any_of(names.begin(), names.end(),
                                   [name](std::string_view n) { return str::iequals(n, name); });
            }

            const VrtElement *vrtElement(std::string_view name)
            {
                const auto &elements = vrtElements();
                const auto it = std::find_if(elements.begin(), elements.end(),
                                             [name](const VrtElement &e) { return str::iequals(e.name, name); });
                return it == elements.end() ? nullptr : &*it;
            }

            // Deeper than any VRT GDAL writes (dataset, band, mask band, band, source, kernel, size)
            constexpr int MaxVrtElementDepth = 16;

            // The bands GDAL reads from their sources; a warped, pansharpened or processed band
            // belongs to datasets that are refused, and a raw band reads any file at an offset
            VrtVerdict validateBandClass(std::string_view subClass)
            {
                if (str::iequals(subClass, "VRTSourcedRasterBand") || str::iequals(subClass, "VRTDerivedRasterBand"))
                    return {true, {}};
                return refuse("it has a band of subClass " + std::string(subClass));
            }

            // GDAL opens the file an item of these domains names (see MetadataFiles), the
            // overviews as early as the open of a VRT with an OverviewList
            VrtVerdict validateMetadataDomain(std::string_view domain)
            {
                for (const auto &file : MetadataFiles)
                    if (str::iequals(domain, file.domain))
                        return refuse(std::string("it has metadata in the ") + file.domain + " domain");
                return {true, {}};
            }

            // The value of an attribute that is listed for its element, where the value matters
            VrtVerdict validateAttribute(const VrtElement &element, std::string_view attribute, std::string_view value)
            {
                if (str::iequals(element.name, "VRTRasterBand") && str::iequals(attribute, "subClass"))
                    return validateBandClass(value);
                if (str::iequals(element.name, "Metadata") && str::iequals(attribute, "domain"))
                    return validateMetadataDomain(value);
                return {true, {}};
            }

            // The first problem in node, an element `rule` describes, or ok
            VrtVerdict validateElement(const CPLXMLNode *node, const VrtElement &rule, const fs::path &vrt,
                                       const fs::path &root, int depth, int elementDepth)
            {
                if (elementDepth > MaxVrtElementDepth)
                    return refuse("its elements are nested deeper than " + std::to_string(MaxVrtElementDepth));

                for (const CPLXMLNode *child = node->psChild; child != nullptr; child = child->psNext)
                {
                    const std::string_view name = child->pszValue;
                    if (child->eType == CXT_Attribute)
                    {
                        if (!rule.anyAttribute && !listed(rule.attributes, name))
                            return refuse("its " + std::string(rule.name) + " has an attribute " + std::string(name) +
                                          ", which a VRT that is opened may not have");
                        if (const auto v = validateAttribute(rule, name, CPLGetXMLValue(child, nullptr, "")); !v.ok)
                            return v;
                    }
                    else if (child->eType == CXT_Element)
                    {
                        const VrtElement *childRule = listed(rule.children, name) ? vrtElement(name) : nullptr;
                        if (childRule == nullptr)
                            return refuse("its " + std::string(rule.name) + " has an element " + std::string(name) +
                                          ", which a VRT that is opened may not have");
                        if (const auto v = validateElement(child, *childRule, vrt, root, depth, elementDepth + 1);
                            !v.ok)
                            return v;
                    }
                    // Text and comments are values, read in place
                }

                if (str::iequals(rule.name, "SourceFilename"))
                    return validateSource(CPLGetXMLValue(node, nullptr, ""), CPLGetXMLValue(node, "relativeToVRT", "0"),
                                          vrt, root, depth);
                return {true, {}};
            }

            VrtVerdict validateVrtAt(const fs::path &path, const fs::path &root, int depth)
            {
                const CPLXMLTreeCloser xml(CPLParseXMLFile(io::toUtf8(path).c_str()));
                if (!xml)
                    return refuse("it is not a readable VRT");

                // GDAL reads the first VRTDataset of the document; every element at its top but
                // the XML declaration must be one
                bool hasDataset = false;
                for (const CPLXMLNode *top = xml.get(); top != nullptr; top = top->psNext)
                {
                    if (top->eType != CXT_Element || std::string_view(top->pszValue) == "?xml")
                        continue;
                    const VrtElement *dataset = vrtElement("VRTDataset");
                    if (!str::iequals(top->pszValue, dataset->name))
                        return refuse("it is not a VRT dataset");
                    if (const auto v = validateElement(top, *dataset, path, root, depth, 0); !v.ok)
                        return v;
                    hasDataset = true;
                }
                if (!hasDataset)
                    return refuse("it is not a VRT dataset");
                return {true, {}};
            }
        } // namespace

        std::vector<std::string> rasterDrivers(const std::string &path)
        {
            return presentDrivers(find(rasterTable(), path));
        }

        std::vector<std::string> vectorDrivers(const std::string &path)
        {
            return presentDrivers(find(vectorTable(), path));
        }

        bool isVrtPath(const std::string &path)
        {
            const auto *entry = find(rasterTable(), path);
            return entry != nullptr && entry->vrt;
        }

        VrtVerdict validateVrt(const fs::path &path, const fs::path &root)
        {
            // Absolute, so a VRT given as a bare file name still has a folder
            return validateVrtAt(fs::absolute(path), fs::absolute(root), 0);
        }

        VrtVerdict validateSidecars(const std::string &path)
        {
            return validateSidecars(path, 0);
        }
    } // namespace gdalopen

    GDALDatasetUniquePtr openUserRaster(const std::string &path)
    {
        const auto drivers = gdalopen::rasterDrivers(path);
        if (drivers.empty())
            return openWith(path, drivers, GDAL_OF_RASTER);
        if (const auto verdict = gdalopen::validateSidecars(path); !verdict.ok)
        {
            LOGW << "Not opening " << path << ": " << verdict.reason;
            return nullptr;
        }
        if (gdalopen::isVrtPath(path))
        {
            // The folder of the VRT is the root its sources must stay in
            const fs::path vrt = io::fromUtf8(path);
            const auto verdict = gdalopen::validateVrt(vrt, fs::absolute(vrt).parent_path());
            if (!verdict.ok)
            {
                LOGW << "Not opening " << path << ": " << verdict.reason;
                return nullptr;
            }
        }
        auto dataset = openWith(path, drivers, GDAL_OF_RASTER);
        // GDAL opens the dataset its metadata names later, when its overviews are first asked
        // for or it is warped: refused before anything can ask
        if (dataset)
        {
            if (const auto verdict = gdalopen::validateMetadataFiles(*dataset); !verdict.ok)
            {
                LOGW << "Not opening " << path << ": " << verdict.reason;
                return nullptr;
            }
        }
        return dataset;
    }

    GDALDriverH identifyUserRaster(const std::string &path)
    {
        const auto drivers = gdalopen::rasterDrivers(path);
        if (drivers.empty())
            return nullptr;
        const CPLStringList allowed = cslFromStrings(drivers);
        return GDALIdentifyDriverEx(path.c_str(), GDAL_OF_RASTER, allowed.List(), nullptr);
    }

    GDALDatasetUniquePtr openUserVector(const std::string &path)
    {
        return openWith(path, gdalopen::vectorDrivers(path), GDAL_OF_VECTOR);
    }
} // namespace ddb
