/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "pointcloud.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <cctype>

#include <pdal/PipelineManager.hpp>
#include <pdal/PointRef.hpp>
#include <pdal/PointTable.hpp>
#include <pdal/StageFactory.hpp>
#include <pdal/io/LasWriter.hpp>
#include <pdal/io/CopcReader.hpp>

#include "entry.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "geo.h"
#include "hash.h"
#include "logger.h"
#include "mio.h"
#include "ply.h"
#include "utils.h"

namespace ddb {

namespace {

// Maximum number of lines to read for format detection
static constexpr size_t MAX_HEADER_LINES = 20;

// Comment prefix used by CloudCompare and other tools
static constexpr const char* COMMENT_PREFIX = "//";

struct TextReaderConfig {
    std::string header;           // PDAL header option (e.g., "X, Y, Z, Red, Green, Blue")
    int skip;                     // PDAL skip option (number of lines to skip)
    char separator;               // PDAL separator option (space, tab, comma, semicolon)
    bool hasHeader;               // Whether a header line was detected (needs header+skip override)
    std::vector<std::string> detectedColumns;
};

// Trim leading/trailing whitespace and surrounding double quotes from a token
static std::string trimAndUnquote(const std::string& token) {
    size_t start = token.find_first_not_of(" \t\r\n");
    size_t end = token.find_last_not_of(" \t\r\n");
    if (start == std::string::npos)
        return "";
    std::string trimmed = token.substr(start, end - start + 1);
    // Strip surrounding double quotes if present
    if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"')
        trimmed = trimmed.substr(1, trimmed.size() - 2);
    return trimmed;
}

// Check if a line is empty or contains only whitespace
static bool isBlankLine(const std::string& line) {
    return line.find_first_not_of(" \t\r\n") == std::string::npos;
}

// Check if a line is a comment (starts with // after optional leading whitespace)
static bool isCommentLine(const std::string& line) {
    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos)
        return false;
    return line.compare(start, 2, COMMENT_PREFIX) == 0;
}

// Check if a string is a single non-negative integer (point count marker)
static bool isSingleInteger(const std::string& line) {
    std::string trimmed = trimAndUnquote(line);
    if (trimmed.empty())
        return false;
    for (char c : trimmed) {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}

// Check if a line contains any alphabetic characters (indicator of a header line)
static bool containsAlpha(const std::string& line) {
    for (char c : line) {
        if (std::isalpha(static_cast<unsigned char>(c)))
            return true;
    }
    return false;
}

// Split a line by a specific separator. For space, any run of whitespace is one separator.
static std::vector<std::string> splitBySeparator(const std::string& line, char sep) {
    std::vector<std::string> result;
    if (sep == ' ') {
        // Whitespace-separated: split on runs of spaces/tabs
        std::istringstream iss(line);
        std::string token;
        while (iss >> token) {
            std::string t = trimAndUnquote(token);
            if (!t.empty())
                result.push_back(t);
        }
    } else {
        std::string token;
        std::istringstream iss(line);
        while (std::getline(iss, token, sep)) {
            std::string t = trimAndUnquote(token);
            if (!t.empty())
                result.push_back(t);
        }
    }
    return result;
}

// Detect the most likely separator from a data/header line among: comma, semicolon, tab, space
static char detectSeparator(const std::string& line) {
    size_t tabCount = 0;
    size_t commaCount = 0;
    size_t semicolonCount = 0;

    for (char c : line) {
        switch (c) {
            case '\t': tabCount++; break;
            case ',': commaCount++; break;
            case ';': semicolonCount++; break;
            default: break;
        }
    }

    // Priority: explicit separators first, fall back to space
    if (commaCount > 0) return ',';
    if (semicolonCount > 0) return ';';
    if (tabCount > 0) return '\t';
    return ' ';
}

// Map common column name variants to PDAL dimension names (alphanumeric + underscore only)
static std::string mapColumnName(const std::string& raw) {
    // Single-letter RGB aliases used by CloudCompare
    if (raw == "R") return "Red";
    if (raw == "G") return "Green";
    if (raw == "B") return "Blue";
    // CloudCompare uses underscores between words; PDAL canonical names are CamelCase
    if (raw == "Return_Number") return "ReturnNumber";
    if (raw == "Number_Of_Returns") return "NumberOfReturns";
    if (raw == "User_Data") return "UserData";
    if (raw == "Scan_Angle_Rank") return "ScanAngleRank";
    if (raw == "Point_Source_ID" || raw == "Point_Source_Id") return "PointSourceId";
    if (raw == "Gps_Time") return "GpsTime";
    if (raw == "Scan_Direction_Flag") return "ScanDirectionFlag";
    if (raw == "Edge_Of_Flight_Line") return "EdgeOfFlightLine";
    if (raw == "Key_Point") return "KeyPoint";
    if (raw == "Scan_Channel") return "ScanChannel";
    return raw;
}

// Read up to maxLines lines from a file into memory
static std::vector<std::string> readLeadingLines(const std::string& filename, size_t maxLines) {
    std::vector<std::string> lines;
    std::ifstream file(filename);
    if (!file.is_open())
        return lines;
    std::string line;
    while (lines.size() < maxLines && std::getline(file, line))
        lines.push_back(line);
    return lines;
}

// Determine if a line "looks like" a header (has alphabetic characters beyond scientific notation).
// Numeric-only data lines may contain 'e'/'E' for scientific notation, but no other letters.
static bool looksLikeHeaderLine(const std::string& line) {
    if (!containsAlpha(line))
        return false;
    // Exclude scientific-notation-only alpha (e/E) followed/preceded by digits or signs
    // Conservative: if the line contains any letter other than e/E, it's a header
    for (char c : line) {
        if (std::isalpha(static_cast<unsigned char>(c)) && c != 'e' && c != 'E')
            return true;
    }
    // Only e/E present: could be scientific notation, not a header
    return false;
}

// Build the PDAL "header" option string from a list of column names using the given separator.
// PDAL parses the header option using the same separator as the data, so they must agree.
static std::string buildHeaderString(const std::vector<std::string>& columns, char sep) {
    std::ostringstream oss;
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) {
            // Use ", " for comma/semicolon to match the common formatting; a plain separator
            // works too, but a trailing space is allowed and improves readability.
            if (sep == ',' || sep == ';')
                oss << sep << ' ';
            else
                oss << sep;
        }
        oss << columns[i];
    }
    return oss.str();
}

// Return a plausible default column list for a data row with `columnCount` values.
// These are best-effort guesses matching conventions of CloudCompare and PDAL writers.
static std::vector<std::string> defaultColumnsForCount(size_t columnCount) {
    // Strictly XYZ only: anything beyond the 3 positional dims is exposed as generic extras.
    // For typical layouts we use names PDAL recognizes natively so downstream tools see colors.
    static const std::vector<std::string> tier3 = {"X", "Y", "Z"};
    static const std::vector<std::string> tier4 = {"X", "Y", "Z", "Intensity"};
    static const std::vector<std::string> tier6 = {"X", "Y", "Z", "Red", "Green", "Blue"};
    static const std::vector<std::string> tier7 = {"X", "Y", "Z", "Red", "Green", "Blue", "Intensity"};
    static const std::vector<std::string> tier9 = {"X", "Y", "Z", "Red", "Green", "Blue",
                                                   "ReturnNumber", "NumberOfReturns", "UserData"};

    switch (columnCount) {
        case 3: return tier3;
        case 4: return tier4;
        case 6: return tier6;
        case 7: return tier7;
        case 9: return tier9;
        default: break;
    }

    // Fallback: X, Y, Z and generic ExtraN dimensions for the remainder
    std::vector<std::string> cols = tier3;
    for (size_t i = 3; i < columnCount; ++i)
        cols.push_back("Extra" + std::to_string(i - 3));
    return cols;
}

// Detect the format of an XYZ text file and return the PDAL configuration needed to read it.
// Reliability-first heuristic:
//   1. If the first line is a "//"-prefixed comment containing alphabetic tokens, treat the
//      content after "//" as the declared header (CloudCompare "columns title" export).
//   2. Skip arbitrary leading comment and blank lines.
//   3. If the next line is a single integer, treat it as a point-count marker and skip it.
//   4. If the next line contains alphabetic tokens, treat it as an in-file header: extract
//      column names, strip quotes, map CloudCompare aliases to PDAL names, and skip it.
//   5. Detect the separator (comma, semicolon, tab, or space) from the header/data line.
//   6. Always populate `config.header` so PDAL never falls back to interpreting a data row
//      as the header line.
TextReaderConfig detectTextFormat(const std::string& filename) {
    TextReaderConfig config;
    config.skip = 0;
    config.separator = ' ';
    config.hasHeader = false;
    config.header = "X, Y, Z";

    const std::vector<std::string> lines = readLeadingLines(filename, MAX_HEADER_LINES);
    if (lines.empty()) {
        LOGD << "Cannot read lines for format detection: " << filename;
        return config;
    }

    // Step 1: attempt to extract column names from the LAST comment line preceding data
    // (CloudCompare "columns title" export places column names in the comment immediately
    // above the data rows; any earlier comments are typically generator/date banners).
    std::vector<std::string> commentColumns;
    size_t lastCommentIdx = std::string::npos;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (isBlankLine(lines[i]))
            continue;
        if (!isCommentLine(lines[i]))
            break;
        lastCommentIdx = i;
    }
    if (lastCommentIdx != std::string::npos) {
        const std::string& line = lines[lastCommentIdx];
        size_t start = line.find_first_not_of(" \t");
        std::string content = line.substr(start + 2);  // after "//"
        if (looksLikeHeaderLine(content)) {
            const char commentSeparator = detectSeparator(content);
            commentColumns = splitBySeparator(content, commentSeparator);
            for (auto& c : commentColumns)
                c = mapColumnName(c);
        }
    }

    // Step 2: skip leading comment/blank lines
    size_t idx = 0;
    while (idx < lines.size() && (isCommentLine(lines[idx]) || isBlankLine(lines[idx])))
        ++idx;
    config.skip = static_cast<int>(idx);

    if (idx >= lines.size()) {
        LOGD << "No data lines found in XYZ file: " << filename;
        return config;
    }

    // Step 3: point-count marker
    if (isSingleInteger(lines[idx])) {
        ++config.skip;
        ++idx;
    }

    if (idx >= lines.size()) {
        LOGD << "No header/data lines after skip in XYZ file: " << filename;
        return config;
    }

    // Step 4: inline header detection
    const std::string& candidate = lines[idx];
    std::vector<std::string> inlineColumns;
    char sep = detectSeparator(candidate);
    if (looksLikeHeaderLine(candidate)) {
        inlineColumns = splitBySeparator(candidate, sep);
        for (auto& c : inlineColumns)
            c = mapColumnName(c);
        ++config.skip;  // Skip the in-file header line, we override via `header` option
        ++idx;
    }

    // Step 5: determine the data separator from the first real data row when available;
    // fall back to whatever we detected from the header candidate.
    if (idx < lines.size() && !isBlankLine(lines[idx]))
        sep = detectSeparator(lines[idx]);
    config.separator = sep;

    // Step 6: choose the best available column list, in priority order:
    //   inline in-file header > comment-line header (validated by data column count) >
    //   inferred from data column count
    const size_t dataColCount =
        (idx < lines.size()) ? splitBySeparator(lines[idx], sep).size() : 0;

    std::vector<std::string> columns;
    if (inlineColumns.size() >= 3) {
        columns = inlineColumns;
    } else if (commentColumns.size() >= 3 &&
               (dataColCount == 0 || commentColumns.size() == dataColCount)) {
        columns = commentColumns;
    } else if (dataColCount > 0) {
        columns = defaultColumnsForCount(dataColCount);
    } else {
        columns = {"X", "Y", "Z"};
    }

    config.hasHeader = true;
    config.detectedColumns = columns;
    config.header = buildHeaderString(columns, sep);

    LOGD << "XYZ format detected for " << filename << ": columns=" << columns.size()
         << ", separator='" << (sep == ' ' ? std::string("space")
                                           : sep == '\t' ? std::string("tab")
                                                         : std::string(1, sep))
         << "', skip=" << config.skip
         << ", header='" << config.header << "'";

    return config;
}

std::string resolvePointCloudReaderDriver(const std::string& filename) {
    const io::Path path(filename);

    if (path.checkExtension({"pts"}))
        return "readers.pts";

    if (path.checkExtension({"xyz"}))
        return "readers.text";

    return pdal::StageFactory::inferReaderDriver(filename);
}

bool suppressDirectSpatialInfo(const io::Path& path) {
    return path.checkExtension({"pts", "xyz"});
}

void clearSpatialInfo(PointCloudInfo& info) {
    info.bounds.clear();
    info.centroid.clear();
    info.polyBounds.clear();
}

void populatePointCloudDimensions(pdal::PointLayoutPtr layout, PointCloudInfo& info) {
    info.dimensions.clear();

    for (pdal::Dimension::Id dimension : layout->dims())
        info.dimensions.push_back(layout->dimName(dimension));
}

void populatePointCloudInfoFromQuickInfo(const pdal::QuickInfo& quickInfo, PointCloudInfo& info) {
    info.pointCount = quickInfo.m_pointCount;
    info.wktProjection = quickInfo.m_srs.valid() ? quickInfo.m_srs.getWKT() : "";

    info.dimensions.clear();
    for (const auto& dimension : quickInfo.m_dimNames)
        info.dimensions.push_back(dimension);
}

void populatePointCloudBoundsFromQuickInfo(const pdal::QuickInfo& quickInfo,
                                           PointCloudInfo& info,
                                           int polyBoundsSrs) {
    clearSpatialInfo(info);
    if (!quickInfo.m_bounds.valid())
        return;

    info.bounds.push_back(quickInfo.m_bounds.minx);
    info.bounds.push_back(quickInfo.m_bounds.miny);
    info.bounds.push_back(quickInfo.m_bounds.minz);
    info.bounds.push_back(quickInfo.m_bounds.maxx);
    info.bounds.push_back(quickInfo.m_bounds.maxy);
    info.bounds.push_back(quickInfo.m_bounds.maxz);

    pdal::BOX3D bbox = quickInfo.m_bounds;

    // We need to convert the bbox to EPSG:<polyboundsSrs>
    if (quickInfo.m_srs.valid()) {
        OGRSpatialReferenceH hSrs = OSRNewSpatialReference(nullptr);
        OGRSpatialReferenceH hTgt = OSRNewSpatialReference(nullptr);

        std::string proj = quickInfo.m_srs.getProj4();
        if (OSRImportFromProj4(hSrs, proj.c_str()) != OGRERR_NONE) {
            OSRDestroySpatialReference(hTgt);
            OSRDestroySpatialReference(hSrs);
            throw GDALException("Cannot import spatial reference system " + proj +
                                ". Is PROJ available?");
        }

        if (OSRImportFromEPSG(hTgt, polyBoundsSrs) != OGRERR_NONE) {
            OSRDestroySpatialReference(hTgt);
            OSRDestroySpatialReference(hSrs);
            throw GDALException("Cannot read EPSG:" + std::to_string(polyBoundsSrs) +
                                ". Is PROJ available?");
        }

        OGRCoordinateTransformationH hTransform = OCTNewCoordinateTransformation(hSrs, hTgt);

        if (!hTransform) {
            OSRDestroySpatialReference(hTgt);
            OSRDestroySpatialReference(hSrs);
            throw GDALException("Cannot create coordinate transformation from " + proj +
                                " to EPSG:" + std::to_string(polyBoundsSrs));
        }

        double geoMinX = bbox.minx;
        double geoMinY = bbox.miny;
        double geoMinZ = bbox.minz;
        double geoMaxX = bbox.maxx;
        double geoMaxY = bbox.maxy;
        double geoMaxZ = bbox.maxz;

        bool minSuccess = OCTTransform(hTransform, 1, &geoMinX, &geoMinY, &geoMinZ);
        bool maxSuccess = OCTTransform(hTransform, 1, &geoMaxX, &geoMaxY, &geoMaxZ);

        if (!minSuccess || !maxSuccess) {
            OCTDestroyCoordinateTransformation(hTransform);
            OSRDestroySpatialReference(hTgt);
            OSRDestroySpatialReference(hSrs);
            throw GDALException("Cannot transform coordinates " + bbox.toWKT() + " to " +
                                proj);
        }

        info.polyBounds.clear();

        if (geoMinZ < -30000 || geoMaxZ > 30000 || (geoMinX == -90 && geoMaxX == 90)) {
            LOGD << "Strange point cloud bounds [[" << geoMinX << ", " << geoMaxX << "], ["
                 << geoMinY << ", " << geoMaxY << "], [" << geoMinZ << ", " << geoMaxZ
                 << "]]";
            info.bounds.clear();
        } else {
            info.polyBounds.addPoint(geoMinX, geoMinY, geoMinZ);
            info.polyBounds.addPoint(geoMaxX, geoMinY, geoMinZ);
            info.polyBounds.addPoint(geoMaxX, geoMaxY, geoMinZ);
            info.polyBounds.addPoint(geoMinX, geoMaxY, geoMinZ);
            info.polyBounds.addPoint(geoMinX, geoMinY, geoMinZ);

            double centroidX = (bbox.minx + bbox.maxx) / 2.0;
            double centroidY = (bbox.miny + bbox.maxy) / 2.0;
            double centroidZ = bbox.minz;

            if (OCTTransform(hTransform, 1, &centroidX, &centroidY, &centroidZ)) {
                info.centroid.clear();
                info.centroid.addPoint(centroidX, centroidY, centroidZ);
            } else {
                OCTDestroyCoordinateTransformation(hTransform);
                OSRDestroySpatialReference(hTgt);
                OSRDestroySpatialReference(hSrs);
                throw GDALException("Cannot transform coordinates " +
                                    std::to_string(centroidX) + ", " +
                                    std::to_string(centroidY) + " to " + proj);
            }
        }

        OCTDestroyCoordinateTransformation(hTransform);
        OSRDestroySpatialReference(hTgt);
        OSRDestroySpatialReference(hSrs);
    }
}

pdal::Stage* createPointCloudReader(pdal::StageFactory& factory, const std::string& filename) {
    const std::string driver = resolvePointCloudReaderDriver(filename);
    if (driver.empty())
        return nullptr;

    pdal::Stage* reader = factory.createStage(driver);
    if (!reader)
        throw PDALException("Cannot create reader stage " + driver + " for " + filename);

    pdal::Options options;
    options.add("filename", filename);

    // Apply heuristic detection for readers.text (XYZ files)
    if (driver == "readers.text") {
        TextReaderConfig config = detectTextFormat(filename);

        // Always set the header explicitly when we've parsed one, so we control dimension naming
        // and PDAL does not fall over quoted/aliased names it cannot canonicalize.
        if (config.hasHeader)
            options.add("header", config.header);

        if (config.skip > 0)
            options.add("skip", config.skip);

        // Only set separator if it's not the default (space). PDAL treats runs of whitespace
        // as a single separator when unset, which is what we want for space-separated files.
        if (config.separator != ' ')
            options.add("separator", std::string(1, config.separator));
    }

    reader->setOptions(options);

    return reader;
}

bool populatePointCloudInfoFromReaderExecution(pdal::Stage& reader, PointCloudInfo& info) {
    pdal::PointTable table;
    reader.prepare(table);

    pdal::PointViewSet pointViewSet = reader.execute(table);
    if (pointViewSet.empty())
        return false;

    info.pointCount = 0;
    for (const auto& pointView : pointViewSet)
        info.pointCount += pointView->size();

    info.wktProjection = "";
    populatePointCloudDimensions(table.layout(), info);
    clearSpatialInfo(info);

    return true;
}

}  // namespace

bool getPointCloudInfo(const std::string& filename, PointCloudInfo& info, int polyBoundsSrs) {
    const io::Path path(filename);

    if (path.checkExtension({"ply"})) {
        PlyInfo plyInfo;
        if (!getPlyInfo(filename, plyInfo))
            return false;
        else {
            info.bounds.clear();
            info.polyBounds.clear();
            info.pointCount = plyInfo.vertexCount;
            info.dimensions = plyInfo.dimensions;
            return true;
        }
    }

    // Las/Laz
    try {
        pdal::StageFactory factory;
        pdal::Stage* reader = createPointCloudReader(factory, filename);
        if (!reader) {
            LOGD << "Can't infer point cloud reader from " << filename;
            return false;
        }

        if (path.checkExtension({"pts"})) {
            if (!populatePointCloudInfoFromReaderExecution(*reader, info)) {
                LOGD << "Cannot read point cloud info for " << filename;
                return false;
            }

            return true;
        }

        pdal::QuickInfo qi = reader->preview();
        if (!qi.valid()) {
            LOGD << "Cannot get quick info for point cloud " << filename;
            return false;
        }

        populatePointCloudInfoFromQuickInfo(qi, info);

        if (suppressDirectSpatialInfo(path))
            clearSpatialInfo(info);
        else
            populatePointCloudBoundsFromQuickInfo(qi, info, polyBoundsSrs);
    } catch (pdal::pdal_error& e) {
        LOGD << "PDAL Error: " << e.what();
        throw PDALException(e.what());
    }

    return true;
}

bool getCopcInfo(const std::string& copcPath, PointCloudInfo& info, int polyBoundsSrs, int* span) {
    try {
        pdal::Options copcOpts;
        copcOpts.add("filename", copcPath);

        pdal::CopcReader reader;
        reader.setOptions(copcOpts);

        pdal::QuickInfo qi = reader.preview();
        if (!qi.valid()) {
            LOGD << "Cannot get quick info for COPC file " << copcPath;
            return false;
        }

        populatePointCloudInfoFromQuickInfo(qi, info);
        populatePointCloudBoundsFromQuickInfo(qi, info, polyBoundsSrs);

        // COPC tile-zoom heuristic: writers.copc emits hierarchies whose root tile is
        // sized to a power-of-two voxel grid; PDAL's default is 128 along each side.
        // This matches the conventional EPT span used elsewhere in the code.
        if (span != nullptr)
            *span = 128;

        return true;
    } catch (const pdal::pdal_error& e) {
        LOGD << "PDAL error reading COPC " << copcPath << ": " << e.what();
        return false;
    } catch (const std::exception& e) {
        LOGD << "Error reading COPC " << copcPath << ": " << e.what();
        return false;
    }
}

// Helper function to update global bounds from point cloud info
inline void updateGlobalBounds(const PointCloudInfo& info,
                               double& globalMinX, double& globalMinY,
                               double& globalMaxX, double& globalMaxY) {
    if (info.bounds.size() >= 6) {
        globalMinX = std::min(globalMinX, info.bounds[0]);
        globalMinY = std::min(globalMinY, info.bounds[1]);
        globalMaxX = std::max(globalMaxX, info.bounds[3]);
        globalMaxY = std::max(globalMaxY, info.bounds[4]);
    }
}

bool isCopcPath(const std::string& filename) {
    if (filename.size() < 9) return false;
    // Lowercase comparison of trailing ".copc.laz"
    static const std::string suffix = ".copc.laz";
    if (filename.size() < suffix.size()) return false;
    const std::string tail = filename.substr(filename.size() - suffix.size());
    std::string lc(tail.size(), '\0');
    std::transform(tail.begin(), tail.end(), lc.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lc == suffix;
}

void buildCopc(const std::vector<std::string>& filenames, const std::string& outdir) {
    fs::path dest = outdir;
    io::assureFolderExists(dest);

    fs::path tmpDir = dest / "tmp";
    io::assureFolderExists(tmpDir);

    // Minimum epsilon for extent validation (in the point cloud's native units)
    // This prevents degenerate output for zero-extent or near-zero-extent inputs.
    constexpr double MIN_EXTENT_EPSILON = 1e-10;
    constexpr size_t MIN_POINT_COUNT = 2;

    size_t totalPointCount = 0;
    double globalMinX = std::numeric_limits<double>::max();
    double globalMinY = std::numeric_limits<double>::max();
    double globalMaxX = std::numeric_limits<double>::lowest();
    double globalMaxY = std::numeric_limits<double>::lowest();

    for (const std::string& f : filenames) {
        if (!fs::exists(f))
            throw FSException(f + " does not exist");

        const EntryType type = fingerprint(f);
        if (type != PointCloud)
            throw InvalidArgsException(f + " is not a supported point cloud file");

        // Get point cloud info for validation
        PointCloudInfo pcInfo;
        if (getPointCloudInfo(f, pcInfo)) {
            totalPointCount += pcInfo.pointCount;
            updateGlobalBounds(pcInfo, globalMinX, globalMinY, globalMaxX, globalMaxY);
        }
    }

    if (totalPointCount < MIN_POINT_COUNT) {
        throw InvalidArgsException("Point cloud has insufficient points (" +
                                   std::to_string(totalPointCount) +
                                   "). At least " + std::to_string(MIN_POINT_COUNT) +
                                   " points are required for COPC generation.");
    }

    // Convert non-LAS sources to a temporary LAS first; PDAL writers.copc accepts only
    // formats supported by its readers (LAS/LAZ natively).
    std::vector<std::string> inputFiles;
    for (const auto& f : filenames) {
        auto p = io::Path(f);
        if (p.checkExtension({"ply", "e57", "pts", "xyz"})) {
            std::string lasF = (tmpDir / Hash::strCRC64(f)).string() + ".las";
            LOGD << "Converting " << f << " to " << lasF;
            translateToLas(f, lasF);
            inputFiles.push_back(lasF);

            // PLY/PTS/XYZ/E57 don't have reliable bounds in their headers; read them
            // back from the converted LAS so the extent validation has something to chew on.
            PointCloudInfo lasInfo;
            if (getPointCloudInfo(lasF, lasInfo)) {
                updateGlobalBounds(lasInfo, globalMinX, globalMinY, globalMaxX, globalMaxY);
            }
        } else {
            inputFiles.push_back(f);
        }
    }

    double extentX = globalMaxX - globalMinX;
    double extentY = globalMaxY - globalMinY;
    if (extentX < MIN_EXTENT_EPSILON || extentY < MIN_EXTENT_EPSILON) {
        throw InvalidArgsException("Point cloud has zero or near-zero extent (X: " +
                                   std::to_string(extentX) + ", Y: " + std::to_string(extentY) +
                                   "). Point cloud must have spatial extent for COPC generation.");
    }

    const fs::path outPath = dest / CopcFileName;
    io::assureIsRemoved(outPath);

    // Legacy EPT cleanup: if a previous EPT build is in this folder, remove it. The
    // build pipeline already does this on its side, but we are defensive here too.
    io::assureIsRemoved(dest / "ept.json");
    io::assureIsRemoved(dest / "ept-data");
    io::assureIsRemoved(dest / "ept-hierarchy");

    try {
        // Build a PDAL pipeline: one or more LAS readers -> writers.copc.
        // When there's a single input we attach the writer directly; when there are
        // multiple inputs, PDAL's writers.copc expects a single stream, which we get
        // by chaining all readers as inputs of a no-op merge filter.
        pdal::PipelineManager mgr;

        std::vector<pdal::Stage*> readers;
        for (const auto& f : inputFiles) {
            pdal::Options ro;
            ro.add("filename", f);
            pdal::Stage& r = mgr.makeReader(f, "readers.las", ro);
            readers.push_back(&r);
        }

        pdal::Stage* tail = readers.front();
        if (readers.size() > 1) {
            pdal::Stage& merge = mgr.makeFilter("filters.merge");
            for (auto* r : readers) merge.setInput(*r);
            tail = &merge;
        }

        pdal::Options wo;
        wo.add("filename", outPath.string());
        wo.add("forward", "all");
        // Preserve any extra dimensions present in the source.
        wo.add("extra_dims", "all");
        pdal::Stage& writer = mgr.makeWriter(outPath.string(), "writers.copc", *tail, wo);
        (void)writer;

        mgr.execute();

        io::assureIsRemoved(tmpDir);
    } catch (const pdal::pdal_error& e) {
        io::assureIsRemoved(tmpDir);
        throw PDALException(std::string("COPC build failed: ") + e.what());
    } catch (const std::exception& e) {
        io::assureIsRemoved(tmpDir);
        throw PDALException(std::string("COPC build failed: ") + e.what());
    }

    if (!fs::exists(outPath))
        throw PDALException("COPC build did not produce output file: " + outPath.string());
}

json PointCloudInfo::toJSON() {
    json j;
    j["pointCount"] = pointCount;
    j["projection"] = wktProjection;
    j["dimensions"] = dimensions;

    return j;
}

// Iterates a point view and returns an array with normalized 8bit colors
std::vector<PointColor> normalizeColors(const std::shared_ptr<pdal::PointView>& point_view) {
    std::vector<PointColor> result;

    // Check if color dimensions exist in the point cloud
    pdal::Dimension::IdList dims = point_view->dims();
    bool hasRed = std::find(dims.begin(), dims.end(), pdal::Dimension::Id::Red) != dims.end();
    bool hasGreen = std::find(dims.begin(), dims.end(), pdal::Dimension::Id::Green) != dims.end();
    bool hasBlue = std::find(dims.begin(), dims.end(), pdal::Dimension::Id::Blue) != dims.end();

    // If color dimensions don't exist, return default gray colors
    if (!hasRed || !hasGreen || !hasBlue) {
        for (pdal::PointId idx = 0; idx < point_view->size(); ++idx) {
            PointColor color;
            color.r = 128; // Default gray color
            color.g = 128;
            color.b = 128;
            result.push_back(color);
        }
        return result;
    }

    bool normalize = false;
    try {
        for (pdal::PointId idx = 0; idx < point_view->size(); ++idx) {
            auto p = point_view->point(idx);
            uint16_t red = p.getFieldAs<uint16_t>(pdal::Dimension::Id::Red);
            uint16_t green = p.getFieldAs<uint16_t>(pdal::Dimension::Id::Green);
            uint16_t blue = p.getFieldAs<uint16_t>(pdal::Dimension::Id::Blue);

            if (red > 255 || green > 255 || blue > 255) {
                normalize = true;
                break;
            }
        }
    } catch (const std::exception& e) {
        LOGD << "Exception in normalizeColors first pass: " << e.what();
        // Return default colors if there's an error accessing color fields
        for (pdal::PointId idx = 0; idx < point_view->size(); ++idx) {
            PointColor color;
            color.r = 128;
            color.g = 128;
            color.b = 128;
            result.push_back(color);
        }
        return result;
    }

    try {
        for (pdal::PointId idx = 0; idx < point_view->size(); ++idx) {
            auto p = point_view->point(idx);
            uint16_t red = p.getFieldAs<uint16_t>(pdal::Dimension::Id::Red);
            uint16_t green = p.getFieldAs<uint16_t>(pdal::Dimension::Id::Green);
            uint16_t blue = p.getFieldAs<uint16_t>(pdal::Dimension::Id::Blue);
            PointColor color;

            if (normalize) {
                color.r = red >> 8;
                color.g = green >> 8;
                color.b = blue >> 8;
            } else {
                color.r = static_cast<uint8_t>(red);
                color.g = static_cast<uint8_t>(green);
                color.b = static_cast<uint8_t>(blue);
            }

            result.push_back(color);
        }
    } catch (const std::exception& e) {
        LOGD << "Exception in normalizeColors second pass: " << e.what();
        // Return default colors if there's an error accessing color fields
        result.clear();
        for (pdal::PointId idx = 0; idx < point_view->size(); ++idx) {
            PointColor color;
            color.r = 128;
            color.g = 128;
            color.b = 128;
            result.push_back(color);
        }
    }

    return result;
}

std::vector<PointColor> generateZBasedColors(const std::shared_ptr<pdal::PointView>& point_view, double minZ, double maxZ) {
    std::vector<PointColor> colors;
    colors.reserve(point_view->size());

    double zRange = maxZ - minZ;

    for (pdal::PointId idx = 0; idx < point_view->size(); ++idx) {
        auto p = point_view->point(idx);
        auto z = p.getFieldAs<double>(pdal::Dimension::Id::Z);

        PointColor color;
        if (zRange > 0) {
            // Normalize Z to 0-1 range
            double normalizedZ = (z - minZ) / zRange;
            // Simple grayscale mapping: lower Z = darker, higher Z = lighter
            uint8_t intensity = static_cast<uint8_t>(normalizedZ * 255);
            color.r = intensity;
            color.g = intensity;
            color.b = intensity;
        } else {
            // All points at same Z level, use gray
            color.r = 128;
            color.g = 128;
            color.b = 128;
        }
        colors.push_back(color);
    }

    return colors;
}

void translateToLas(const std::string& input, const std::string& outputLas) {
    if (!fs::exists(input))
        throw FSException(input + " does not exist");

    try {
        pdal::StageFactory factory;
        pdal::Stage* reader = createPointCloudReader(factory, input);
        if (!reader)
            throw PDALException("Cannot infer reader driver for " + input);

        pdal::PointTable table;
        pdal::Options outLasOpts;
        outLasOpts.add("filename", outputLas);
        outLasOpts.add("minor_version", 2);
        outLasOpts.add("dataformat_id", 3);

        pdal::LasWriter writer;
        writer.setOptions(outLasOpts);
        writer.setInput(*reader);
        writer.prepare(table);
        writer.execute(table);
    } catch (pdal::pdal_error& e) {
        throw PDALException(e.what());
    }
}

}  // namespace ddb
