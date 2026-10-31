/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "cad.h"
#include "stringutil.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <locale>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>

#include "exceptions.h"
#include "gdal_raii.h"
#include "logger.h"
#include "mio.h"
#include "process_runner.h"
#include "raster_helper.h"
#include "tool_locator.h"
#include "tool_run_support.h"

namespace ddb
{
    namespace cad
    {
        namespace
        {
            constexpr const char *kConverterEnv = "DDB_DWG2DXF_PATH";
            constexpr const char *kMaxSecondsEnv = "DDB_DWG2DXF_MAX_SECONDS";
            constexpr auto kDefaultMaxRuntime = std::chrono::seconds(120);
            constexpr auto kIdleTimeout = std::chrono::seconds(60);
            const std::string kDxfAdvice =
                ". Export the drawing as DXF from your CAD software and upload that.";

            // The features of the layers a vector probe lists (the layers without an extent, which
            // have no feature, are not listed)
            GIntBig featureCount(const raster::ProbeResult &probed)
            {
                GIntBig features = 0;
                const auto vector = probed.properties.find("vector");
                if (vector == probed.properties.end() || !vector->is_object() || !vector->contains("layers"))
                    return features;
                for (const auto &layer : (*vector)["layers"])
                    features += std::max<int64_t>(0, layer.value("featureCount", int64_t{0}));
                return features;
            }

            // DXF caret escapes in string values (^J newline, ^M CR, ^I tab, "^ " = ^).
            std::string decodeCaret(const std::string &v)
            {
                std::string out;
                out.reserve(v.size());
                for (size_t i = 0; i < v.size(); i++)
                {
                    if (v[i] == '^' && i + 1 < v.size())
                    {
                        const char n = v[i + 1];
                        if (n == 'J') { out.push_back('\n'); i++; continue; }
                        if (n == 'M') { out.push_back('\r'); i++; continue; }
                        if (n == 'I') { out.push_back('\t'); i++; continue; }
                        if (n == ' ') { out.push_back('^'); i++; continue; }
                    }
                    out.push_back(v[i]);
                }
                return out;
            }

            bool parseCode(const std::string &line, int &code)
            {
                const std::string t = str::trimCopy(line);
                if (t.empty()) return false;
                size_t i = (t[0] == '-') ? 1 : 0;
                if (i >= t.size()) return false;
                for (size_t j = i; j < t.size(); j++)
                    if (!str::isDigitChar(t[j])) return false;
                code = std::atoi(t.c_str());
                return true;
            }

            std::string srsLabel(OGRSpatialReference &s)
            {
                const char *an = s.GetAuthorityName(nullptr);
                const char *ac = s.GetAuthorityCode(nullptr);
                if ((!an || !ac) && s.AutoIdentifyEPSG() == OGRERR_NONE)
                {
                    an = s.GetAuthorityName(nullptr);
                    ac = s.GetAuthorityCode(nullptr);
                }
                if (an && ac) return std::string(an) + ":" + ac;
                const char *name = s.GetName();
                return name ? name : "";
            }

            std::string toWkt(const OGRSpatialReference &s)
            {
                char *wkt = nullptr;
                const char *options[] = {"FORMAT=WKT2_2019", nullptr};
                s.exportToWkt(&wkt, options);
                return takeGdalString(wkt);
            }
        } // namespace

        bool isCadFile(const fs::path &p)
        {
            return io::Path(p).checkExtension({"dxf", "dwg"});
        }

        bool isDwgFile(const fs::path &p)
        {
            return io::Path(p).checkExtension({"dwg"});
        }

        fs::path findDwg2DxfBinary(bool forceRefresh)
        {
            return tools::findTool("dwg2dxf", kConverterEnv, forceRefresh);
        }

        std::string dwgVersion(const fs::path &dwg)
        {
            char magic[6] = {0};
            std::ifstream in(dwg, std::ios::binary);
            in.read(magic, sizeof(magic));
            const auto got = in.gcount();
            if (got < 6 || magic[0] != 'A' || magic[1] != 'C' ||
                !str::isDigitChar(magic[2]))
                throw AppException(io::toUtf8(dwg.filename()) + " is not a valid DWG file");

            const std::string code(magic, 6);
            static const std::pair<const char *, const char *> names[] = {
                {"AC1009", "R11/R12"}, {"AC1012", "R13"},   {"AC1014", "R14"},
                {"AC1015", "R2000"},   {"AC1018", "R2004"}, {"AC1021", "R2007"},
                {"AC1024", "R2010"},   {"AC1027", "R2013"}, {"AC1032", "R2018"}};
            for (const auto &[c, n] : names)
                if (code == c) return code + " (" + n + ")";
            return code;
        }

        LibreDwgConverter::LibreDwgConverter(fs::path binaryPath, const process::IProcessRunner &processRunner)
            : binary(std::move(binaryPath)), runner(&processRunner) {}

        void LibreDwgConverter::toDxf(const fs::path &dwg, const fs::path &outDxf) const
        {
            const std::string version = dwgVersion(dwg);

            const fs::path bin = binary.empty() ? findDwg2DxfBinary() : binary;
            if (!tools::isExecutableFile(bin))
                throw AppException(std::string("DWG support needs dwg2dxf (LibreDWG); set ") +
                                   kConverterEnv + " or install dwg2dxf next to ddb");

            process::Options opts;
            opts.idleTimeout = kIdleTimeout;
            opts.maxRuntime = tools::toolMaxRuntime(kMaxSecondsEnv, kDefaultMaxRuntime);

            LOGD << "Converting " << io::toUtf8(dwg) << " (" << version << ") to DXF with " << io::toUtf8(bin);
            const auto r = runner->run(bin, {"-y", "-o", io::toUtf8(outDxf), io::toUtf8(dwg)}, opts);
            if (!r.success())
            {
                io::assureIsRemoved(outDxf);
                throw AppException("DWG conversion failed (" + version + "): " + tools::oneLine(r.failureText(300)) +
                                   kDxfAdvice);
            }

            // dwg2dxf exits 0 on files it could not decode: only a DXF that GDAL opens and that
            // has features counts as converted. The DXF is what the drawing of a user became, so
            // the raster helper reads it, not this process.
            raster::ProbeParams probeParams;
            probeParams.kind = raster::ProbeKind::Vector;
            probeParams.source = outDxf;
            probeParams.converted = true;
            probeParams.sidecarDecided = true;
            raster::ProbeResult probed;
            try
            {
                probed = raster::rasterHelper().probe(outDxf, probeParams);
            }
            catch (const GDALException &)
            {
                io::assureIsRemoved(outDxf);
                throw AppException("DWG conversion failed (" + version +
                                   "): the converted drawing cannot be read" + kDxfAdvice);
            }
            // A drawing whose coordinate system GDAL cannot use is described in part (its
            // layers are not listed): it was read, so it is not a failed conversion
            const GIntBig features = featureCount(probed);
            if (features == 0 && probed.warning.empty())
            {
                io::assureIsRemoved(outDxf);
                throw AppException("DWG conversion failed (" + version +
                                   "): the drawing has no 2D features that can be read (for example, a drawing of 3D solids only)" +
                                   kDxfAdvice);
            }
            LOGD << "Converted " << io::toUtf8(dwg.filename()) << ": " << features << " features";
        }

        CadSource::CadSource(const fs::path &input, const ICadConverter *converter, const fs::path &workDir)
        {
            if (!isDwgFile(input))
            {
                dxf = input;
                return;
            }

            // A member: removed when the conversion throws, as when the source is destroyed
            const fs::path parent = workDir.empty() ? fs::temp_directory_path() : workDir;
            tempDir = io::ScopedTempDir::create(parent, "ddb-cad-");
            dxf = tempDir.path() / io::concat(input.stem(), ".dxf");

            if (converter)
                converter->toDxf(input, dxf);
            else
                LibreDwgConverter().toDxf(input, dxf);
        }

        CadSource::~CadSource() = default;

        // A group of the GEODATA object: its code says which field of info (or of the definition: the
        // type of the first group 70, the XML of the groups 3 and 301) the value is
        void readGeoDataValue(DxfInfo &info, int code, const std::string &value, int &geoType, std::string &geoXml)
        {
            const auto number = [&value]() { return std::atof(str::trimCopy(value).c_str()); };
            switch (code)
            {
            case 70: if (geoType < 0) geoType = std::atoi(str::trimCopy(value).c_str()); break;
            case 90: info.geoDataVersion = std::atoi(str::trimCopy(value).c_str()); break;
            case 3: case 301: geoXml += decodeCaret(value); break;
            case 10: info.geoDesignX = number(); break;
            case 20: info.geoDesignY = number(); break;
            case 11: info.geoReferenceX = number(); break;
            case 21: info.geoReferenceY = number(); break;
            case 12: info.geoNorthX = number(); break;
            case 22: info.geoNorthY = number(); break;
            case 40: info.geoUnitScale = number(); break;
            default: break;
            }
        }

        DxfInfo readDxfInfo(const fs::path &dxf)
        {
            DxfInfo info;
            std::ifstream in(dxf, std::ios::binary);
            if (!in.is_open()) return info;

            char head[18] = {0};
            in.read(head, sizeof(head));
            if (in.gcount() == sizeof(head) && std::string(head, sizeof(head)) == "AutoCAD Binary DXF")
                return info; // binary DXF: not supported here (no CRS, no units)
            in.clear();
            in.seekg(0);

            enum class Section { None, Header, Objects, Other };
            Section section = Section::None;
            bool expectSectionName = false;
            bool expectInsUnits = false;
            bool inGeoData = false, geoDataDone = false;
            std::string geoXml;
            int geoType = -1;

            std::string codeLine, value;
            while (std::getline(in, codeLine))
            {
                int code = 0;
                if (!parseCode(codeLine, code))
                {
                    // A value that spanned lines broke the pairing: resynchronise by
                    // treating this line as a value and the next one as a code.
                    if (inGeoData) { inGeoData = false; geoDataDone = !geoXml.empty(); }
                    continue;
                }
                if (!std::getline(in, value)) break;
                if (!value.empty() && value.back() == '\r') value.pop_back();

                if (code == 0)
                {
                    if (inGeoData)
                    {
                        inGeoData = false;
                        geoDataDone = !geoXml.empty() || geoType >= 0;
                    }
                    const std::string v = str::trimCopy(value);
                    if (v == "SECTION") { expectSectionName = true; continue; }
                    if (v == "ENDSEC")
                    {
                        if (section == Section::Objects) break; // nothing after OBJECTS is needed
                        section = Section::None;
                        continue;
                    }
                    if (section == Section::Objects && v == "GEODATA" && !geoDataDone)
                    {
                        inGeoData = true;
                        geoXml.clear();
                        geoType = -1;
                    }
                    continue;
                }

                if (expectSectionName && code == 2)
                {
                    const std::string v = str::trimCopy(value);
                    section = v == "HEADER" ? Section::Header : v == "OBJECTS" ? Section::Objects : Section::Other;
                    expectSectionName = false;
                    continue;
                }

                if (section == Section::Header)
                {
                    if (code == 9) expectInsUnits = str::trimCopy(value) == "$INSUNITS";
                    else if (code == 70 && expectInsUnits)
                    {
                        info.insUnits = std::atoi(str::trimCopy(value).c_str());
                        expectInsUnits = false;
                    }
                }
                else if (inGeoData)
                    readGeoDataValue(info, code, value, geoType, geoXml);
            }

            if (geoType >= 0 || !geoXml.empty())
            {
                info.geoDataType = geoType;
                info.geoDataDefinition = geoXml;
            }
            return info;
        }

        std::string insUnitsName(int insUnits)
        {
            switch (insUnits)
            {
            case 1: return "in";
            case 2: return "ft";
            case 3: return "mi";
            case 4: return "mm";
            case 5: return "cm";
            case 6: return "m";
            case 7: return "km";
            case 10: return "yd";
            case 14: return "dm";
            case 21: return "ft (US survey)";
            default: return "";
            }
        }

        std::optional<std::string> srsFromGeoData(int type, const std::string &definition)
        {
            if ((type != 2 && type != 3) || definition.empty()) return std::nullopt;

            // AutoCAD/Civil 3D write an EPSG alias for the coordinate system:
            // <Alias id="2193" type="CoordinateSystem"> ... <Namespace>EPSG Code</Namespace>
            static const std::regex aliasRe(R"re(<Alias\b([^>]*)>([\s\S]*?)</Alias>)re");
            static const std::regex idRe(R"re(\bid\s*=\s*"(\d+)")re");
            for (auto it = std::sregex_iterator(definition.begin(), definition.end(), aliasRe);
                 it != std::sregex_iterator(); ++it)
            {
                const std::string attrs = (*it)[1].str();
                const std::string body = (*it)[2].str();
                if (attrs.find("type=\"CoordinateSystem\"") == std::string::npos) continue;
                if (body.find("<Namespace>EPSG Code</Namespace>") == std::string::npos) continue;
                std::smatch m;
                if (!std::regex_search(attrs, m, idRe)) continue;
                OGRSpatialReference s;
                if (s.importFromEPSG(std::atoi(m[1].str().c_str())) == OGRERR_NONE)
                    return "EPSG:" + m[1].str();
            }

            // Older or third-party writers: the definition itself may be WKT.
            OGRSpatialReference s;
            if (setSrsFromUserInput(s, definition.c_str()) == OGRERR_NONE)
                return toWkt(s);
            return std::nullopt;
        }

        namespace
        {
            // The text of the WKT node starting at `start` ("NAME[...]"), brackets matched
            std::string wktNode(const std::string &wkt, size_t start)
            {
                int depth = 0;
                bool quoted = false;
                for (size_t i = start; i < wkt.size(); i++)
                {
                    if (wkt[i] == '"') quoted = !quoted;
                    else if (!quoted && wkt[i] == '[') depth++;
                    else if (!quoted && wkt[i] == ']' && --depth == 0)
                        return wkt.substr(start, i - start + 1);
                }
                return "";
            }

            std::string wktNumber(double v)
            {
                std::ostringstream os;
                os.imbue(std::locale::classic());
                os << std::setprecision(17) << v;
                return os.str();
            }

            // WKT2 of a CRS derived from projected `base` by an affine conversion
            // (EPSG:9624): drawing = A0 + A1 E + A2 N, B0 + B1 E + B2 N
            std::optional<std::string> localGridCrs(const OGRSpatialReference &base, const DxfInfo &info)
            {
                const std::string wkt = toWkt(base);
                const auto geog = wkt.find("BASEGEOGCRS[");
                const auto conversion = wkt.find("CONVERSION[");
                // PROJCRS["name",...: the name is the first quoted string
                const auto nameEnd = wkt.find('"', 9);
                if (!wkt.starts_with("PROJCRS[\"") || geog == std::string::npos ||
                    conversion == std::string::npos || nameEnd == std::string::npos)
                    return std::nullopt;
                const std::string name = wkt.substr(8, nameEnd - 7);

                const char *unitName = nullptr;
                const double unitMetres = base.GetLinearUnits(&unitName);
                const double northLength = std::hypot(info.geoNorthX, info.geoNorthY);
                if (!(info.geoUnitScale > 0) || !(unitMetres > 0) || !(northLength > 0))
                    return std::nullopt;

                // grid = reference + s R (drawing - design), R turning the drawing's north
                // to grid north (counter-clockwise by phi); the conversion is the inverse
                const double s = info.geoUnitScale / unitMetres;
                const double phi = std::atan2(info.geoNorthX / northLength, info.geoNorthY / northLength);
                const double c = std::cos(phi), sn = std::sin(phi);
                const double a1 = c / s, a2 = sn / s, b1 = -sn / s, b2 = c / s;
                const double a0 = info.geoDesignX - (a1 * info.geoReferenceX + a2 * info.geoReferenceY);
                const double b0 = info.geoDesignY - (b1 * info.geoReferenceX + b2 * info.geoReferenceY);

                const std::string unit = "LENGTHUNIT[\"" + std::string(unitName ? unitName : "metre") + "\"," +
                                         wktNumber(unitMetres) + "]";
                const auto parameter = [](const char *n, double v, const std::string &u) {
                    return std::string("PARAMETER[\"") + n + "\"," + wktNumber(v) + "," + u + "]";
                };
                const std::string coefficient = "SCALEUNIT[\"coefficient\",1]";
                const std::string localName = "Local grid of " + name.substr(1, name.size() - 2);

                return "DERIVEDPROJCRS[\"" + localName + "\",BASEPROJCRS[" + name + "," + wktNode(wkt, geog) + "," +
                       wktNode(wkt, conversion) + "],DERIVINGCONVERSION[\"Drawing local grid\"," +
                       "METHOD[\"Affine parametric transformation\",ID[\"EPSG\",9624]]," +
                       parameter("A0", a0, unit) + "," + parameter("A1", a1, coefficient) + "," +
                       parameter("A2", a2, coefficient) + "," + parameter("B0", b0, unit) + "," +
                       parameter("B1", b1, coefficient) + "," + parameter("B2", b2, coefficient) + "]," +
                       "CS[Cartesian,2],AXIS[\"(E)\",east,ORDER[1]," + unit + "],AXIS[\"(N)\",north,ORDER[2]," +
                       unit + "]]";
            }
        } // namespace

        std::string prjContent(const std::string &crs)
        {
            const std::string input = str::trimCopy(crs);
            if (input == "local")
                return "LOCAL_CS[\"Drawing\"]";

            OGRSpatialReference s;
            if (input.empty() || setSrsFromUserInput(s, input.c_str()) != OGRERR_NONE)
                throw InvalidArgsException("Not a coordinate system: " + input);
            if (s.IsLocal())
                return "LOCAL_CS[\"Drawing\"]";

            char *wkt = nullptr;
            const char *options[] = {"FORMAT=WKT1_ESRI", nullptr};
            const auto err = s.exportToWkt(&wkt, options);
            std::string out = takeGdalString(wkt);
            if (err != OGRERR_NONE || out.empty())
            {
                // A CRS without an ESRI form (e.g. a derived CRS): WKT2 still reads back
                out = toWkt(s);
                if (out.empty())
                    throw InvalidArgsException("Cannot write the coordinate system " + input);
            }
            return out;
        }

        std::optional<std::string> srsFromGeoData(const DxfInfo &info)
        {
            if (info.geoDataType != 1)
                return srsFromGeoData(info.geoDataType, info.geoDataDefinition);

            // Local grid: the definition names the coordinate system of the reference point.
            // GEODATA version 1 (AutoCAD 2009) lays the points out differently: not read.
            if (info.geoDataVersion < 2)
                return std::nullopt;
            const auto base = srsFromGeoData(2, info.geoDataDefinition);
            if (!base)
                return std::nullopt;
            OGRSpatialReference s;
            if (setSrsFromUserInput(s, base->c_str()) != OGRERR_NONE || !s.IsProjected())
                return std::nullopt;
            return localGridCrs(s, info);
        }

        namespace
        {
            thread_local std::vector<fs::path> t_ignoredSidecars;

            fs::path normalized(const fs::path &p)
            {
                std::error_code ec;
                const auto abs = fs::absolute(p, ec);
                return (ec ? p : abs).lexically_normal();
            }
        } // namespace

        IgnoredSidecarScope::IgnoredSidecarScope(std::vector<fs::path> files) : previous(t_ignoredSidecars)
        {
            for (const auto &f : files)
                t_ignoredSidecars.push_back(normalized(f));
        }

        IgnoredSidecarScope::~IgnoredSidecarScope()
        {
            t_ignoredSidecars = std::move(previous);
        }

        std::optional<fs::path> findPrjSidecar(const fs::path &file)
        {
            for (const char *ext : {".prj", ".PRJ", ".Prj"})
            {
                fs::path p = file;
                p.replace_extension(io::fromUtf8(ext));
                std::error_code ec;
                if (!fs::is_regular_file(p, ec)) continue;
                const auto n = normalized(p);
                bool ignored = false;
                // equivalent(): on a case-insensitive file system ".PRJ" names the
                // ignored ".prj" too
                for (const auto &i : t_ignoredSidecars)
                {
                    std::error_code eq;
                    ignored = ignored || i == n || fs::equivalent(i, p, eq);
                }
                if (!ignored) return p;
            }
            return std::nullopt;
        }

        CadCrs resolveCadCrs(const fs::path &originalInput, GDALDatasetH ds, const DxfInfo &info)
        {
            CadCrs crs;
            OGRSpatialReferenceH layerSrs = nullptr;

            for (int i = 0; i < GDALDatasetGetLayerCount(ds); i++)
            {
                OGRLayerH layer = GDALDatasetGetLayer(ds, i);
                if (!layer) continue;
                if (!layerSrs) layerSrs = OGR_L_GetSpatialRef(layer);
                OGREnvelope e;
                if (OGR_L_GetExtent(layer, &e, TRUE) != OGRERR_NONE) continue;
                if (!crs.hasExtent) crs.extent = e;
                else crs.extent.Merge(e);
                crs.hasExtent = true;
            }

            // 1. A layer SRS (a future GDAL may read one from the drawing).
            if (layerSrs)
            {
                crs.georeferenced = true;
                crs.source = "layer";
                // Copy: AutoIdentifyEPSG must not modify the layer's own SRS.
                OGRSpatialReference s(*OGRSpatialReference::FromHandle(layerSrs));
                crs.label = srsLabel(s);
                return crs;
            }

            // 2. An optional same-name .prj.
            if (const auto prj = findPrjSidecar(originalInput))
            {
                std::ifstream in(*prj, std::ios::binary);
                std::stringstream ss;
                ss << in.rdbuf();
                const std::string content = str::trimCopy(ss.str());
                OGRSpatialReference s;
                if (content.empty() ||
                    (setSrsFromUserInput(s, content.c_str()) != OGRERR_NONE &&
                     setSrsFromUserInput(s, ("ESRI::" + content).c_str()) != OGRERR_NONE))
                    throw AppException("Cannot read the coordinate system in " + io::toUtf8(prj->filename()));
                crs.source = "prj";
                // A local (engineering) CRS, e.g. LOCAL_CS["Drawing"], says "these are
                // drawing coordinates": the explicit way to keep a drawing off the map.
                if (s.IsLocal())
                {
                    crs.georeferenced = false;
                    crs.label = "local";
                    return crs;
                }
                crs.georeferenced = true;
                crs.srs = toWkt(s);
                crs.label = srsLabel(s);
                return crs;
            }

            // 3. GEODATA stored in the drawing by AutoCAD / Civil 3D.
            if (const auto srs = srsFromGeoData(info))
            {
                OGRSpatialReference s;
                // For the label only (srsLabel of an empty SRS is ""): the CRS is the string itself
                setSrsFromUserInput(s, srs->c_str());
                crs.georeferenced = true;
                crs.source = "geodata";
                crs.srs = *srs;
                crs.label = srsLabel(s);
                return crs;
            }

            // 4. DXF coordinates that can be degrees keep the historical EPSG:4326
            // assumption: GIS tools export lon/lat as DXF without any CRS. Never for DWG,
            // which only CAD software writes (it stores GEODATA when georeferenced): the
            // DWG drawings that fit in +-180/+-90 were design drawings in drawing units.
            if (!isDwgFile(originalInput) && crs.hasExtent &&
                crs.extent.MinX >= -180 && crs.extent.MaxX <= 180 &&
                crs.extent.MinY >= -90 && crs.extent.MaxY <= 90)
            {
                crs.georeferenced = true;
                crs.source = "lonlat";
                crs.srs = "EPSG:4326";
                crs.label = "EPSG:4326";
                return crs;
            }

            // 5. Local drawing coordinates.
            crs.georeferenced = false;
            crs.source = "none";
            return crs;
        }
    }
}
