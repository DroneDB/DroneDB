/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "utils.h"
#include "randomutil.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <boolinq/boolinq.h>

#include "dbops.h"
#include "exceptions.h"
#include "ddb.h"
#include "gdal_inc.h"
#include "gtest/gtest.h"
#include "hash.h"
#include "json.h"
#include "mio.h"

#include <zip.h>

static OGRSpatialReference srsOfEpsg(int epsg)
{
    OGRSpatialReference srs;
    if (srs.importFromEPSG(epsg) != OGRERR_NONE)
        throw ddb::GDALException("Cannot import spatial reference system " + std::to_string(epsg));
    return srs;
}

EpsgTransformer::EpsgTransformer(int epsgFrom, int epsgTo)
    : ddb::CoordsTransformer(srsOfEpsg(epsgFrom), srsOfEpsg(epsgTo))
{
}

std::vector<ddb::VegetationFormula> availableFormulas(const ddb::BandFilter &filter, const ddb::RasterDataKind &data)
{
    std::vector<ddb::VegetationFormula> result;
    for (const auto &formula : ddb::VegetationEngine::instance().getAllFormulas())
    {
        if (ddb::VegetationEngine::unavailableReason(formula, filter, data).empty())
            result.push_back(formula);
    }
    return result;
}

std::filesystem::path writeFile(const std::filesystem::path &path, const std::string &content)
{
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
    return path;
}

std::filesystem::path writeFile(const std::filesystem::path &path, const std::vector<uint8_t> &content)
{
    return writeFile(path, std::string(content.begin(), content.end()));
}

void zipFolder(const std::string &folder, const std::string &zipFile)
{
    int error = 0;
    zip_t *archive = zip_open(zipFile.c_str(), ZIP_CREATE | ZIP_EXCL | ZIP_TRUNCATE, &error);
    if (archive == nullptr)
        throw std::runtime_error("Cannot create zip file " + zipFile + " (libzip error " + std::to_string(error) + ")");

    try
    {
        const auto root = ddb::io::fromUtf8(folder);
        for (const auto &item : fs::recursive_directory_iterator(root))
        {
            const auto name = ddb::io::Path(item.path()).relativeTo(root).generic();
            if (fs::is_directory(item.path()))
            {
                if (zip_dir_add(archive, name.c_str(), ZIP_FL_ENC_UTF_8) < 0)
                    throw std::runtime_error(std::string("Cannot add directory to zip: ") + zip_strerror(archive));
                continue;
            }

            zip_source_t *source = zip_source_file(archive, ddb::io::toUtf8(item.path()).c_str(), 0, 0);
            if (source == nullptr)
                throw std::runtime_error(std::string("Cannot add file to zip: ") + zip_strerror(archive));
            if (zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_UTF_8) < 0)
            {
                zip_source_free(source);
                throw std::runtime_error(std::string("Cannot add file to zip: ") + zip_strerror(archive));
            }
        }

        if (zip_close(archive) != 0)
            throw std::runtime_error(std::string("Cannot close zip file: ") + zip_strerror(archive));
    }
    catch (...)
    {
        zip_discard(archive);
        throw;
    }
}

void fileWriteAllText(const fs::path &path, const std::string &content)
{
    if (exists(path))
        fs::remove(path);

    writeFile(path, content);
}

fs::path makeTree(const std::vector<ddb::SimpleEntry> &entries)
{
    const auto tempFolder =
        fs::temp_directory_path() / "diff_test" / ddb::randomString(8);

    create_directories(tempFolder);

    const auto sortedEntries =
        boolinq::from(entries)
            .orderBy([](const ddb::SimpleEntry &entry)
                     { return entry.path; })
            .toStdVector();

    for (const auto &entry : sortedEntries)
    {
        const auto relPath = tempFolder / entry.path;
        if (!entry.isDirectory())
        {
            fileWriteAllText(relPath, entry.hash);
        }
        else
        {
            create_directories(relPath);
        }
    }

    return tempFolder;
}

bool compareTree(fs::path &sourceFolder, fs::path &destFolder)
{
    const auto source = getEntries(sourceFolder);
    const auto dest = getEntries(destFolder);

    std::cout << "Source Folder Tree" << std::endl;

    printTree(sourceFolder);

    std::cout << std::endl
              << "Dest Folder Tree" << std::endl;

    printTree(destFolder);

    std::cout << std::endl;

    return source == dest;
}
void printTree(fs::path &folder)
{
    std::vector<fs::path> entries;

    std::cout << "PrintTree: " << folder << std::endl;

    for (const auto &item : fs::recursive_directory_iterator(folder))
    {
        entries.push_back(item.path());
    }
    const auto sortedEntries =
        boolinq::from(entries)
            .orderBy([](const fs::path &entry)
                     { return entry.generic_string(); })
            .toStdVector();

    for (const auto &path : sortedEntries)
    {

        const auto rel = relative(path, folder);

        const auto relString = rel.generic_string();

        const auto depth = std::count(
            relString.begin(), relString.end(),
            '/');

        for (auto n = 0; n < depth; n++)
            std::cout << "\t";

        std::cout << rel.filename();

        if (fs::is_regular_file(relString))
            std::cout << " (" << calculateHash(path) << ")";

        std::cout << std::endl;
    }
}

std::vector<ddb::SimpleEntry> getEntries(const fs::path &path)
{
    std::vector<ddb::SimpleEntry> entries;

    const auto pathLength = path.generic_string().length();

    for (const auto &item : fs::recursive_directory_iterator(path))
    {
        auto fileName = item.path().generic_string();
        const auto itm = item.path().generic_string();

        if (fileName.size() > pathLength + 1)
            fileName = fileName.substr(pathLength + 1);

        if (item.is_directory())
            entries.emplace_back(fileName);
        else
            entries.emplace_back(fileName, calculateHash(itm));
    }

    return entries;
}

std::string calculateHash(const fs::path &file)
{
    return ddb::Hash::fileSHA256(file.string());
}

void createFile(const fs::path& filePath) {
    std::ofstream file(filePath);
    if (file) {
        std::cout << "Created file: " << filePath << std::endl;
    } else {
        std::cerr << "Failed to create file: " << filePath << std::endl;
    }
}

bool createTestTree(const fs::path& root) {
    try {
        // Define the structure
        fs::path folderA = root / "folderA";
        fs::path testTxtA = folderA / "test.txt";
        fs::path folderB = folderA / "folderB";
        fs::path testTxtB = folderB / "test.txt";

        // Create directories
        fs::create_directories(folderB);
        std::cout << "Created directories: " << folderB << std::endl;

        // Create files
        createFile(testTxtA);
        createFile(testTxtB);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return false;
    }
    return true;

}

std::string toHumanReadableTime(long long timeMs) {

    long long minutes = timeMs / 60000;
    long long seconds = (timeMs % 60000) / 1000;
    long long milliseconds = timeMs % 1000;

    std::string result;
    if (minutes > 0) result += std::to_string(minutes) + "m ";
    if (seconds > 0 || minutes > 0) result += std::to_string(seconds) + "s ";
    result += std::to_string(milliseconds) + "ms";

    return result;

}

IndexFixture::IndexFixture(const std::string &testArea, const std::filesystem::path &folder, bool recreateArea)
    : ta(testArea, recreateArea), root(ta.getFolder(folder))
{
    ddb::initIndex(root.string());
    db = ddb::open(root.string(), false);
}

std::filesystem::path makeIndex(const std::string &testArea, const std::filesystem::path &folder, bool recreateArea)
{
    IndexFixture index(testArea, folder, recreateArea);
    writeFile(index.root / "a.txt", "alpha");
    ddb::addToIndex(index.db.get(), {(index.root / "a.txt").string()});
    return index.root;
}

std::string gridStl(int n, bool binary, bool solidHeader) {
    struct Vertex {
        float x, y, z;
    };
    const auto at = [n](int i, int j) {
        return Vertex{static_cast<float>(i),
                      static_cast<float>(j),
                      static_cast<float>((i + j) % 3) * 0.25f};
    };

    std::vector<std::array<Vertex, 3>> triangles;
    for (int j = 0; j + 1 < n; ++j)
        for (int i = 0; i + 1 < n; ++i) {
            triangles.push_back({at(i, j), at(i + 1, j), at(i + 1, j + 1)});
            triangles.push_back({at(i, j), at(i + 1, j + 1), at(i, j + 1)});
        }

    std::string out;
    if (binary) {
        std::string header = solidHeader ? "solid grid, written as binary" : "binary grid";
        header.resize(80, ' ');
        out = header;
        const uint32_t count = static_cast<uint32_t>(triangles.size());
        out.append(reinterpret_cast<const char*>(&count), sizeof(count));
        for (const auto& t : triangles) {
            const float normal[3] = {0.f, 0.f, 1.f};
            out.append(reinterpret_cast<const char*>(normal), sizeof(normal));
            for (const auto& v : t)
                out.append(reinterpret_cast<const char*>(&v), sizeof(v));
            out.append(2, '\0');
        }
        return out;
    }

    std::ostringstream o;
    o << "solid grid\n";
    for (const auto& t : triangles) {
        o << "  facet normal 0 0 1\n    outer loop\n";
        for (const auto& v : t)
            o << "      vertex " << v.x << " " << v.y << " " << v.z << "\n";
        o << "    endloop\n  endfacet\n";
    }
    o << "endsolid grid\n";
    return o.str();
}

void insertEntry(ddb::Database *db, const std::string &path, const std::string &hash, ddb::EntryType type)
{
    auto q = db->query("INSERT INTO entries (path, hash, type, properties, mtime, size, depth) "
                       "VALUES (?, ?, ?, '{}', 0, 1, ?)");
    q->bind(1, path);
    q->bind(2, hash);
    q->bind(3, static_cast<int>(type));
    q->bind(4, static_cast<int>(std::count(path.begin(), path.end(), '/')));
    q->execute();
}

bool loadEntry(ddb::Database *db, const std::string &path, ddb::Entry &out)
{
    auto entry = ddb::getEntry(db, path);
    if (entry)
        out = std::move(*entry);
    return entry.has_value();
}

bool loadEntryWithMeta(ddb::Database *db, const std::string &path, ddb::Entry &out)
{
    auto entry = ddb::getEntryWithMeta(db, path);
    if (entry)
        out = std::move(*entry);
    return entry.has_value();
}

std::string hashOf(ddb::Database *db, const std::string &path)
{
    ddb::Entry e;
    return loadEntry(db, path, e) ? e.hash : "<missing>";
}

std::string indexedHash(ddb::Database *db, const std::string &path)
{
    ddb::Entry e;
    EXPECT_TRUE(loadEntry(db, path, e)) << path << " is not indexed";
    return e.hash;
}

int openDatasetCount()
{
    GDALDatasetH *datasets = nullptr;
    int count = 0;
    GDALGetOpenDatasets(&datasets, &count);
    return count;
}

void setEnv(const std::string &name, const std::string &value)
{
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

void unsetEnv(const std::string &name)
{
#ifdef _WIN32
    _putenv_s(name.c_str(), "");
#else
    unsetenv(name.c_str());
#endif
}

EnvVarGuard::EnvVarGuard(std::string name) : name_(std::move(name))
{
    if (const char *value = std::getenv(name_.c_str()))
        saved_ = value;
}

EnvVarGuard::EnvVarGuard(std::string name, const std::string &value) : EnvVarGuard(std::move(name))
{
    setEnv(name_, value);
}

EnvVarGuard::~EnvVarGuard()
{
    restore();
}

void EnvVarGuard::restore()
{
    if (restored_)
        return;
    restored_ = true;
    if (saved_)
        setEnv(name_, *saved_);
    else
        unsetEnv(name_);
}

ScopedProcessConfig::ScopedProcessConfig(const std::string &configJson)
{
    const json config = json::parse(configJson);
    for (const auto &item : config.items())
        names_.push_back(item.key());
    EXPECT_EQ(DDBConfigure(configJson.c_str()), DDBERR_NONE) << configJson << ": " << DDBGetLastError();
}

ScopedProcessConfig::~ScopedProcessConfig()
{
    json reset = json::object();
    for (const auto &name : names_)
        reset[name] = nullptr;
    EXPECT_EQ(DDBConfigure(reset.dump().c_str()), DDBERR_NONE) << DDBGetLastError();
}
