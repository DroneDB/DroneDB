/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "cad.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>

#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "process_runner.h"
#include "tool_locator.h"
#include "utils.h"

namespace ddb
{
    namespace cad
    {
        namespace
        {
            constexpr const char *kConverterEnv = "DDB_DWG2DXF_PATH";
            constexpr const char *kMaxSecondsEnv = "DDB_DWG2DXF_MAX_SECONDS";
            constexpr long long kDefaultMaxSeconds = 120;
            constexpr auto kIdleTimeout = std::chrono::seconds(60);
            const std::string kDxfAdvice =
                ". Export the drawing as DXF from your CAD software and upload that.";

            std::string lowerExt(const fs::path &p)
            {
                auto ext = p.extension().string();
                utils::toLower(ext);
                return ext;
            }

            std::chrono::milliseconds maxRuntime()
            {
                long long seconds = kDefaultMaxSeconds;
                if (const char *v = std::getenv(kMaxSecondsEnv); v && *v)
                {
                    char *end = nullptr;
                    const long long parsed = std::strtoll(v, &end, 10);
                    if (end && *end == '\0' && parsed > 0)
                        seconds = parsed;
                    else
                        LOGW << kMaxSecondsEnv << "=" << v << " is not a positive integer; using "
                             << kDefaultMaxSeconds;
                }
                return std::chrono::seconds(seconds);
            }

            // One display line out of a tool's output tail.
            std::string singleLine(std::string s)
            {
                for (auto &c : s)
                    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                return s;
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

            std::string trim(std::string s)
            {
                while (!s.empty() && (s.back() == '\r' || std::isspace(static_cast<unsigned char>(s.back()))))
                    s.pop_back();
                size_t i = 0;
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
                return s.substr(i);
            }

            bool parseCode(const std::string &line, int &code)
            {
                const std::string t = trim(line);
                if (t.empty()) return false;
                size_t i = (t[0] == '-') ? 1 : 0;
                if (i >= t.size()) return false;
                for (size_t j = i; j < t.size(); j++)
                    if (!std::isdigit(static_cast<unsigned char>(t[j]))) return false;
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
                std::string out = wkt ? wkt : "";
                CPLFree(wkt);
                return out;
            }
        } // namespace

        bool isCadFile(const fs::path &p)
        {
            const auto ext = lowerExt(p);
            return ext == ".dxf" || ext == ".dwg";
        }

        bool isDwgFile(const fs::path &p)
        {
            return lowerExt(p) == ".dwg";
        }

        fs::path findDwg2DxfBinary()
        {
            return tools::findTool("dwg2dxf", kConverterEnv);
        }

        std::string dwgVersion(const fs::path &dwg)
        {
            char magic[6] = {0};
            std::ifstream in(dwg.string(), std::ios::binary);
            in.read(magic, sizeof(magic));
            const auto got = in.gcount();
            if (got < 6 || magic[0] != 'A' || magic[1] != 'C' ||
                !std::isdigit(static_cast<unsigned char>(magic[2])))
                throw AppException(dwg.filename().string() + " is not a valid DWG file");

            const std::string code(magic, 6);
            static const std::pair<const char *, const char *> names[] = {
                {"AC1009", "R11/R12"}, {"AC1012", "R13"},   {"AC1014", "R14"},
                {"AC1015", "R2000"},   {"AC1018", "R2004"}, {"AC1021", "R2007"},
                {"AC1024", "R2010"},   {"AC1027", "R2013"}, {"AC1032", "R2018"}};
            for (const auto &[c, n] : names)
                if (code == c) return code + " (" + n + ")";
            return code;
        }

        LibreDwgConverter::LibreDwgConverter(fs::path binary) : binary(std::move(binary)) {}

        void LibreDwgConverter::toDxf(const fs::path &dwg, const fs::path &outDxf) const
        {
            const std::string version = dwgVersion(dwg);

            const fs::path bin = binary.empty() ? findDwg2DxfBinary() : binary;
            if (bin.empty() || !tools::isExecutableFile(bin))
                throw AppException(std::string("DWG support needs dwg2dxf (LibreDWG); set ") +
                                   kConverterEnv + " or install dwg2dxf next to ddb");

            process::Options opts;
            opts.idleTimeout = kIdleTimeout;
            opts.maxRuntime = maxRuntime();

            LOGD << "Converting " << dwg.string() << " (" << version << ") to DXF with " << bin.string();
            const auto r = process::run(bin, {"-y", "-o", outDxf.string(), dwg.string()}, opts);
            if (!r.success())
            {
                io::assureIsRemoved(outDxf);
                std::string detail = r.describe();
                const std::string tail = singleLine(r.tail(300));
                if (!tail.empty()) detail += ": " + tail;
                throw AppException("DWG conversion failed (" + version + "): " + detail + kDxfAdvice);
            }

            // dwg2dxf exits 0 on files it could not decode (DDB-12 RESULTS §2, §5):
            // only a DXF that GDAL opens and that has features counts as converted.
            const char *drivers[] = {"DXF", nullptr};
            GDALDatasetH ds = GDALOpenEx(outDxf.string().c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                         drivers, nullptr, nullptr);
            if (!ds)
            {
                io::assureIsRemoved(outDxf);
                throw AppException("DWG conversion failed (" + version +
                                   "): the converted drawing cannot be read" + kDxfAdvice);
            }
            GIntBig features = 0;
            for (int i = 0; i < GDALDatasetGetLayerCount(ds); i++)
            {
                const GIntBig n = OGR_L_GetFeatureCount(GDALDatasetGetLayer(ds, i), TRUE);
                if (n > 0) features += n;
            }
            GDALClose(ds);
            if (features == 0)
            {
                io::assureIsRemoved(outDxf);
                throw AppException("DWG conversion failed (" + version +
                                   "): the drawing has no 2D features that can be read (for example, a drawing of 3D solids only)" +
                                   kDxfAdvice);
            }
            LOGD << "Converted " << dwg.filename().string() << ": " << features << " features";
        }

        CadSource::CadSource(const fs::path &input, const ICadConverter *converter, const fs::path &workDir)
        {
            if (!isDwgFile(input))
            {
                dxf = input;
                return;
            }

            const fs::path parent = workDir.empty() ? fs::temp_directory_path() : workDir;
            tempDir = parent / ("ddb-cad-" + utils::generateRandomString(16));
            io::assureFolderExists(tempDir);
            dxf = tempDir / (input.stem().string() + ".dxf");

            try
            {
                if (converter)
                    converter->toDxf(input, dxf);
                else
                    LibreDwgConverter().toDxf(input, dxf);
            }
            catch (...)
            {
                io::assureIsRemoved(tempDir);
                tempDir.clear();
                throw;
            }
        }

        CadSource::~CadSource()
        {
            if (tempDir.empty()) return;
            try
            {
                io::assureIsRemoved(tempDir);
            }
            catch (const std::exception &e)
            {
                LOGW << "Cannot remove " << tempDir.string() << ": " << e.what();
            }
        }

        DxfInfo readDxfInfo(const fs::path &dxf)
        {
            DxfInfo info;
            std::ifstream in(dxf.string(), std::ios::binary);
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
                    const std::string v = trim(value);
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
                    const std::string v = trim(value);
                    section = v == "HEADER" ? Section::Header : v == "OBJECTS" ? Section::Objects : Section::Other;
                    expectSectionName = false;
                    continue;
                }

                if (section == Section::Header)
                {
                    if (code == 9) expectInsUnits = trim(value) == "$INSUNITS";
                    else if (code == 70 && expectInsUnits)
                    {
                        info.insUnits = std::atoi(trim(value).c_str());
                        expectInsUnits = false;
                    }
                }
                else if (inGeoData)
                {
                    if (code == 70 && geoType < 0) geoType = std::atoi(trim(value).c_str());
                    else if (code == 3 || code == 301) geoXml += decodeCaret(value);
                }
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
            if (s.SetFromUserInput(definition.c_str()) == OGRERR_NONE)
                return toWkt(s);
            return std::nullopt;
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
                p.replace_extension(ext);
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
                std::ifstream in(prj->string(), std::ios::binary);
                std::stringstream ss;
                ss << in.rdbuf();
                const std::string content = trim(ss.str());
                OGRSpatialReference s;
                if (content.empty() ||
                    (s.SetFromUserInput(content.c_str()) != OGRERR_NONE &&
                     s.SetFromUserInput(("ESRI::" + content).c_str()) != OGRERR_NONE))
                    throw AppException("Cannot read the coordinate system in " + prj->filename().string());
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
            if (const auto srs = srsFromGeoData(info.geoDataType, info.geoDataDefinition))
            {
                OGRSpatialReference s;
                s.SetFromUserInput(srs->c_str());
                crs.georeferenced = true;
                crs.source = "geodata";
                crs.srs = *srs;
                crs.label = srsLabel(s);
                return crs;
            }

            // 4. (D1) DXF coordinates that can be degrees keep the historical EPSG:4326
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
