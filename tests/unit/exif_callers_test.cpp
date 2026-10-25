/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// What the code that works on image metadata does with the metadata reader (exif_metadata.h):
// the index reads each image once and reads nothing else, a file the reader has no metadata for is
// indexed as a plain image, a reader that cannot work stops `ddb add` and the thermal and
// multispectral paths instead of being taken for a file without EXIF, and `ddb setexif` writes
// through the reader, reports a file it cannot write on stderr and goes on with the next one.
// The reader is the real metadata-util (built next to the test binary) unless a test replaces it.

#include <atomic>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cmd/include/add.h"
#include "cmd/include/cli.h"
#include "cmd/include/setexif.h"
#include "dbops.h"
#include "exceptions.h"
#include "exif.h"
#include "exif_helper_pool.h"
#include "exifeditor.h"
#include "gdal_inc.h"
#include "gtest/gtest.h"
#include "hash.h"
#include "merge_multispectral.h"
#include "mio.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "thermal.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    // A small valid JPEG without metadata (GDAL JPEG driver)
    void writeJpeg(const fs::path &path, int width = 16, int height = 16)
    {
        GDALDatasetH memory = GDALCreate(GDALGetDriverByName("MEM"), "", width, height, 3, GDT_Byte, nullptr);
        ASSERT_NE(memory, nullptr);
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height);
        for (int band = 1; band <= 3; ++band)
        {
            for (size_t i = 0; i < pixels.size(); ++i)
                pixels[i] = static_cast<uint8_t>((i * 17 * band) & 0xFF);
            ASSERT_EQ(GDALRasterIO(GDALGetRasterBand(memory, band), GF_Write, 0, 0, width, height, pixels.data(), width,
                                   height, GDT_Byte, 0, 0),
                      CE_None);
        }
        GDALDatasetH out = GDALCreateCopy(GDALGetDriverByName("JPEG"), path.string().c_str(), memory, FALSE, nullptr,
                                          nullptr, nullptr);
        GDALClose(memory);
        ASSERT_NE(out, nullptr);
        GDALClose(out);
        std::error_code ec;
        fs::remove(fs::path(path.string() + ".aux.xml"), ec);
    }

    int countEntries(const fs::path &dataset)
    {
        auto db = ddb::open(dataset.string(), true);
        auto q = db->query("SELECT COUNT(*) FROM entries");
        q->fetch();
        return q->getInt(0);
    }

    // The entries a batch added, by path
    std::map<std::string, Entry> byPath(const AddResult &result)
    {
        std::map<std::string, Entry> entries;
        for (const auto &[entry, updated] : result.entries)
            entries[entry.path] = entry;
        return entries;
    }

    // A reader that counts what it is asked and answers every read with an image of 8 x 8 pixels
    // without tags
    class CountingReader : public exif::IMetadataReader
    {
      public:
        exif::ReadResult read(const fs::path &) override
        {
            ++reads;
            exif::ReadResult result;
            result.metadata = exif::ImageMetadata(8, 8, {}, {});
            return result;
        }
        exif::WriteResult write(const fs::path &, const exif::TagAssignments &) override
        {
            exif::WriteResult result;
            result.ok = true;
            return result;
        }

        std::atomic<size_t> reads{0};
    };

    // A reader that counts the reads of each file and forwards them to the reader of the process,
    // so the answers (no metadata for a file that is not a JPEG) are the real ones
    class ForwardingCountingReader : public exif::IMetadataReader
    {
      public:
        exif::ReadResult read(const fs::path &file) override
        {
            ++reads[file];
            return next_.read(file);
        }
        exif::WriteResult write(const fs::path &file, const exif::TagAssignments &set) override
        {
            return next_.write(file, set);
        }

        std::map<fs::path, size_t> reads;

      private:
        exif::IMetadataReader &next_ = exif::metadataReader();
    };

    // ------------------------------------------------------------------ the index

    TEST(ExifIndex, parsingAnEntryReadsItsMetadataOnceWithOrWithoutMetadata)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        const fs::path bad = root / "bad.jpg";
        const fs::path plain = root / "plain.jpg";
        fileWriteAllText(bad, "this is not a jpeg");
        writeJpeg(plain, 48, 32);

        ForwardingCountingReader reader;
        exif::ScopedMetadataReader scope(reader);

        // The fingerprint reads the file; parsing the entry reuses what it read, including the
        // answer that the file has no metadata
        Entry badEntry;
        parseEntry(bad, root, badEntry, false);
        EXPECT_EQ(badEntry.type, EntryType::Image);
        for (const char *key : {"width", "height", "captureTime", "make"})
            EXPECT_FALSE(badEntry.properties.contains(key)) << key << ": " << badEntry.properties.dump();
        EXPECT_EQ(reader.reads[bad], 1u);

        Entry plainEntry;
        parseEntry(plain, root, plainEntry, false);
        EXPECT_EQ(plainEntry.type, EntryType::Image);
        EXPECT_EQ(plainEntry.properties.value("width", 0), 48);
        EXPECT_EQ(plainEntry.properties.value("height", 0), 32);
        EXPECT_EQ(reader.reads[plain], 1u);

        // A context reused for the next file reads that file afresh
        FingerprintContext ctx;
        Entry first;
        parseEntry(plain, root, first, false, ctx);
        Entry second;
        parseEntry(bad, root, second, false, ctx);
        EXPECT_EQ(second.type, EntryType::Image);
        EXPECT_FALSE(second.properties.contains("width")) << second.properties.dump();
        EXPECT_EQ(reader.reads[plain], 2u);
        EXPECT_EQ(reader.reads[bad], 2u);
    }

    TEST(ExifIndex, aJpgThatIsNotAJpegIsAPlainImageAndTheBatchContinues)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        fileWriteAllText(root / "a.txt", "hello");
        fileWriteAllText(root / "bad.jpg", "this is not a jpeg");
        writeJpeg(root / "plain.jpg", 48, 32);
        writeJpeg(root / "geo.jpg", 48, 32);
        ASSERT_TRUE(ExifEditor(io::toUtf8(root / "geo.jpg")).SetGPS(46.0667, 11.1167, 194.0).empty());

        // On its own: an image by its extension, nothing read from it
        EXPECT_EQ(fingerprint(root / "bad.jpg"), EntryType::Image);

        initIndex(root.string());
        auto db = ddb::open(root.string(), true);
        AddOptions options;
        options.stopOnError = false;
        AddResult result;
        addToIndexEx(db.get(),
                     {(root / "a.txt").string(), (root / "bad.jpg").string(), (root / "plain.jpg").string(),
                      (root / "geo.jpg").string()},
                     options, result);

        EXPECT_TRUE(result.errors.empty()) << (result.errors.empty() ? "" : result.errors.front().message);
        const auto entries = byPath(result);
        ASSERT_EQ(entries.size(), 4u);

        const Entry &bad = entries.at("bad.jpg");
        EXPECT_EQ(bad.type, EntryType::Image);
        // Indexed without EXIF: none of what the tags and the dimensions give
        for (const char *key : {"make", "model", "sensor", "orientation", "width", "height", "captureTime"})
            EXPECT_FALSE(bad.properties.contains(key)) << key << ": " << bad.properties.dump();

        const Entry &plain = entries.at("plain.jpg");
        EXPECT_EQ(plain.type, EntryType::Image);
        EXPECT_EQ(plain.properties.value("width", 0), 48);
        EXPECT_EQ(plain.properties.value("height", 0), 32);

        EXPECT_EQ(entries.at("geo.jpg").type, EntryType::GeoImage);
        EXPECT_EQ(entries.at("a.txt").type, EntryType::Generic);
        EXPECT_EQ(countEntries(root), 4);
    }

    TEST(ExifIndex, theIndexReadsEachImageOnceWithFewHelpers)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        constexpr size_t images = 24;
        std::vector<std::string> paths;
        for (size_t i = 0; i < images; ++i)
        {
            const fs::path image = root / ("img" + std::to_string(i) + ".jpg");
            writeJpeg(image, 16 + static_cast<int>(i), 16);
            paths.push_back(image.string());
        }

        const ScopedProcessConfig config(R"({"cpuPoolSize": 3})");
        exif::HelperMetadataReader pool;
        exif::ScopedMetadataReader scope(pool);

        initIndex(root.string());
        auto db = ddb::open(root.string(), true);
        AddOptions options;
        options.stopOnError = false;
        AddResult result;
        addToIndexEx(db.get(), paths, options, result);

        EXPECT_TRUE(result.errors.empty());
        EXPECT_EQ(result.entries.size(), images);
        // One read request per file (the fingerprint's tags are the ones the entry is parsed from)
        // and no process per file: the pool holds at most the pool size of the process plus one
        EXPECT_EQ(pool.requestsSent(), images);
        EXPECT_GE(pool.startedHelpers(), 1u);
        EXPECT_LE(pool.startedHelpers(), 4u);
    }

    TEST(ExifIndex, manyIndexingThreadsShareOneHelper)
    {
        // Each indexing thread starts the read of a file before it hashes it, and with a single
        // helper most of them find it busy: they hash without it and read when they are done.
        // None may wait for a helper another thread holds while that thread waits for it, and
        // each must get the metadata of its own file.
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        constexpr size_t images = 16;
        std::vector<std::string> paths;
        for (size_t i = 0; i < images; ++i)
        {
            const fs::path image = root / ("img" + std::to_string(i) + ".jpg");
            writeJpeg(image, 16 + static_cast<int>(i), 16);
            if (i % 2 == 0)
            {
                ASSERT_TRUE(ExifEditor(io::toUtf8(image)).SetGPS(46.0 + static_cast<double>(i) * 0.001, 11.0, 194.0).empty());
            }
            paths.push_back(image.string());
        }

        EnvVarGuard threads("DDB_INDEX_THREADS", "8");
        exif::HelperMetadataReaderOptions options;
        options.limit = [] { return size_t(1); };
        exif::HelperMetadataReader pool(options);
        exif::ScopedMetadataReader scope(pool);

        initIndex(root.string());
        auto db = ddb::open(root.string(), true);
        AddOptions addOptions;
        addOptions.stopOnError = false;
        AddResult result;
        addToIndexEx(db.get(), paths, addOptions, result);

        EXPECT_TRUE(result.errors.empty()) << (result.errors.empty() ? "" : result.errors.front().message);
        const auto entries = byPath(result);
        ASSERT_EQ(entries.size(), images);
        for (size_t i = 0; i < images; ++i)
        {
            const Entry &entry = entries.at("img" + std::to_string(i) + ".jpg");
            EXPECT_EQ(entry.type, i % 2 == 0 ? EntryType::GeoImage : EntryType::Image) << entry.path;
            EXPECT_EQ(entry.properties.value("width", 0), 16 + static_cast<int>(i)) << entry.path;
        }
        EXPECT_EQ(pool.requestsSent(), images) << "one read per file";
        EXPECT_EQ(pool.startedHelpers(), 1u);
        EXPECT_EQ(pool.busyHelpers(), 0u);
        EXPECT_EQ(pool.queuedCallers(), 0u);
    }

    TEST(ExifIndex, filesThatAreNotImagesIssueNoRead)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        fileWriteAllText(root / "a.txt", "hello");
        fileWriteAllText(root / "notes.md", "# notes");
        fileWriteAllText(root / "table.csv", "a,b\n1,2\n");
        fileWriteAllText(root / "points.geojson",
                         R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{},"geometry":{"type":"Point","coordinates":[11.1,46.0]}}]})");
        fileWriteAllText(root / "triangle.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 32;
        testutils::createGeoRaster(root / "ortho.tif", spec);

        CountingReader reader;
        exif::ScopedMetadataReader scope(reader);

        initIndex(root.string());
        auto db = ddb::open(root.string(), true);
        AddOptions options;
        options.stopOnError = false;
        AddResult result;
        addToIndexEx(db.get(), {root.string()}, options, result);

        EXPECT_TRUE(result.errors.empty()) << (result.errors.empty() ? "" : result.errors.front().message);
        const auto entries = byPath(result);
        EXPECT_EQ(entries.at("ortho.tif").type, EntryType::GeoRaster);
        EXPECT_EQ(entries.at("points.geojson").type, EntryType::Vector);
        EXPECT_EQ(reader.reads.load(), 0u);

        // The counter works: an image is one read
        writeJpeg(root / "photo.jpg");
        AddResult second;
        addToIndexEx(db.get(), {(root / "photo.jpg").string()}, options, second);
        ASSERT_EQ(second.entries.size(), 1u);
        EXPECT_EQ(reader.reads.load(), 1u);
    }

    // ------------------------------------------------------------------ reads that run while a file is hashed

    // A reader that records, per file, what it was asked: "begin" (a read started), "get" (its
    // result taken) and "read". Every answer is "no metadata". With `rewrite`, it changes the
    // file when a read starts and again when its result is taken, so the hash of the entry tells
    // when the file was hashed.
    class OrderRecorder : public exif::IMetadataReader
    {
      public:
        static constexpr const char *kWhilePending = "the file while its read is pending";
        static constexpr const char *kAfterTaken = "the file after the result of its read was taken";

        explicit OrderRecorder(bool rewrite) : rewrite_(rewrite) {}

        exif::PendingRead begin(const fs::path &file) override
        {
            record(file, "begin", kWhilePending);
            return exif::PendingRead(std::make_unique<Pending>(*this, file));
        }
        exif::ReadResult read(const fs::path &file) override
        {
            record(file, "read", nullptr);
            return noMetadata();
        }
        exif::WriteResult write(const fs::path &, const exif::TagAssignments &) override { return {}; }

        std::vector<std::string> eventsOf(const fs::path &file)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = events_.find(file.filename().string());
            return it == events_.end() ? std::vector<std::string>() : it->second;
        }
        size_t files()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return events_.size();
        }

      private:
        class Pending : public exif::PendingRead::Request
        {
          public:
            Pending(OrderRecorder &owner, fs::path file) : owner_(owner), file_(std::move(file)) {}
            exif::ReadResult get() override
            {
                owner_.record(file_, "get", kAfterTaken);
                return noMetadata();
            }

          private:
            OrderRecorder &owner_;
            fs::path file_;
        };

        static exif::ReadResult noMetadata()
        {
            exif::ReadResult result;
            result.message = "recorded";
            return result;
        }

        void record(const fs::path &file, const char *event, const char *content)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            events_[file.filename().string()].push_back(event);
            if (rewrite_ && content != nullptr)
                fileWriteAllText(file, content);
        }

        const bool rewrite_;
        std::mutex mutex_;
        std::map<std::string, std::vector<std::string>> events_;
    };

    using Events = std::vector<std::string>;

    TEST(ExifIndex, theReadOfAnImageStartsBeforeItIsHashedAndIsTakenAfter)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        writeJpeg(root / "photo.jpg");
        writeJpeg(root / "unhashed.jpg");
        writeJpeg(root / "hashed.jpg");

        OrderRecorder reader(true);
        exif::ScopedMetadataReader scope(reader);

        Entry entry;
        parseEntry(root / "photo.jpg", root, entry, true);
        EXPECT_EQ(reader.eventsOf(root / "photo.jpg"), (Events{"begin", "get"}));
        // What was hashed is the file as it was in between
        EXPECT_EQ(entry.hash, Hash::strSHA256(OrderRecorder::kWhilePending));
        EXPECT_EQ(entry.type, EntryType::Image);

        // Without hashing, or with the hash known already, there is nothing to overlap: a read
        Entry unhashed;
        parseEntry(root / "unhashed.jpg", root, unhashed, false);
        EXPECT_EQ(reader.eventsOf(root / "unhashed.jpg"), (Events{"read"}));
        Entry hashed;
        hashed.hash = "known";
        parseEntry(root / "hashed.jpg", root, hashed, true);
        EXPECT_EQ(reader.eventsOf(root / "hashed.jpg"), (Events{"read"}));
        EXPECT_EQ(hashed.hash, "known");
    }

    TEST(ExifIndex, onlyTheFilesDetectionAlwaysReadsAreReadWhileTheyAreHashed)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        fileWriteAllText(root / "a.txt", "hello");
        fileWriteAllText(root / "points.geojson",
                         R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{},"geometry":{"type":"Point","coordinates":[11.1,46.0]}}]})");
        fileWriteAllText(root / "triangle.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 32;
        testutils::createGeoRaster(root / "ortho.tif", spec);
        fileWriteAllText(root / "plain.tif", "a TIFF without a projection, as far as GDAL can tell");
        writeJpeg(root / "photo.jpg");
        fileWriteAllText(root / "clip.mp4", "a video");

        OrderRecorder reader(false);
        exif::ScopedMetadataReader scope(reader);

        initIndex(root.string());
        auto db = ddb::open(root.string(), true);
        AddOptions options;
        options.stopOnError = false;
        AddResult result;
        addToIndexEx(db.get(), {root.string()}, options, result);
        EXPECT_TRUE(result.errors.empty()) << (result.errors.empty() ? "" : result.errors.front().message);
        EXPECT_EQ(result.entries.size(), 7u);

        // Not read at all, not even started
        for (const char *name : {"a.txt", "points.geojson", "triangle.obj", "ortho.tif"})
            EXPECT_TRUE(reader.eventsOf(root / name).empty()) << name;
        // A TIFF is read only when GDAL finds no projection in it, so its read starts no earlier
        EXPECT_EQ(reader.eventsOf(root / "plain.tif"), (Events{"read"}));
        // Images and videos: one read each, started before the hash
        EXPECT_EQ(reader.eventsOf(root / "photo.jpg"), (Events{"begin", "get"}));
        EXPECT_EQ(reader.eventsOf(root / "clip.mp4"), (Events{"begin", "get"}));
        EXPECT_EQ(reader.files(), 3u);
    }

    // Starts each read on the pool, then removes the file, so hashing it fails while the read is
    // pending
    class RemovingReader : public exif::IMetadataReader
    {
      public:
        explicit RemovingReader(exif::HelperMetadataReader &pool) : pool_(pool) {}
        exif::PendingRead begin(const fs::path &file) override
        {
            auto pending = pool_.begin(file);
            fs::remove(file);
            return pending;
        }
        exif::ReadResult read(const fs::path &file) override { return pool_.read(file); }
        exif::WriteResult write(const fs::path &file, const exif::TagAssignments &set) override
        {
            return pool_.write(file, set);
        }

      private:
        exif::HelperMetadataReader &pool_;
    };

    TEST(ExifIndex, anImageThatCannotBeHashedLeavesNoHelperHeld)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        writeJpeg(root / "gone.jpg");
        writeJpeg(root / "next.jpg");

        exif::HelperMetadataReader::Options options;
        options.limit = [] { return size_t(1); };
        exif::HelperMetadataReader pool(options);
        RemovingReader reader(pool);
        exif::ScopedMetadataReader scope(reader);

        // A context the caller keeps across the failure
        FingerprintContext ctx;
        Entry gone;
        EXPECT_THROW(parseEntry(root / "gone.jpg", root, gone, true, ctx), FSException);
        EXPECT_FALSE(ctx.pendingRead.valid());
        EXPECT_EQ(pool.requestsSent(), 1u);
        EXPECT_EQ(pool.idleHelpers(), 0u) << "the helper with the unread response is not reused";
        // With room for one helper, a held place would make the next read wait forever
        ASSERT_EQ(pool.busyHelpers(), 0u);

        // RemovingReader removes what it reads: the next file is read straight from the pool
        exif::ScopedMetadataReader direct(pool);
        Entry next;
        parseEntry(root / "next.jpg", root, next, true, ctx);
        EXPECT_EQ(next.type, EntryType::Image);
        EXPECT_EQ(next.properties.value("width", 0), 16);
        EXPECT_EQ(pool.requestsSent(), 2u);
        EXPECT_EQ(pool.startedHelpers(), 2u);
        EXPECT_EQ(pool.idleHelpers(), 1u);
        EXPECT_EQ(pool.busyHelpers(), 0u);
    }

    // ------------------------------------------------------------------ a reader that cannot work

    // The ways metadata-util can be missing: a file that is not there, and a file that cannot run
    std::vector<std::string> unusableHelpers(const fs::path &folder)
    {
        std::vector<std::string> paths = {io::toUtf8(folder / "no-such-metadata-util")};
#ifndef _WIN32
        const fs::path notExecutable = folder / "metadata-util-text";
        fileWriteAllText(notExecutable, "not a program");
        fs::permissions(notExecutable, fs::perms::owner_read | fs::perms::owner_write);
        paths.push_back(io::toUtf8(notExecutable));
#endif
        return paths;
    }

    TEST(ExifIndex, addWithoutTheHelperFailsNamesItAndAddsNothing)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        writeJpeg(root / "photo.jpg");
        initIndex(root.string());

        for (const auto &program : unusableHelpers(ta.getFolder("tools")))
        {
            SCOPED_TRACE(program);
            const EnvVarGuard env(exif::kMetadataUtilEnv, program);
            exif::HelperMetadataReader pool;
            exif::ScopedMetadataReader scope(pool);

            cmd::Add add;
            std::string workingDir = io::toUtf8(root);
            std::string file = io::toUtf8(root / "photo.jpg");
            std::vector<char *> argv{const_cast<char *>("ddb"), const_cast<char *>("-w"), workingDir.data(), file.data()};
            std::ostringstream err;
            std::ostringstream out;
            std::streambuf *previous = std::cout.rdbuf(out.rdbuf());
            const int code = cmd::execute(add, static_cast<int>(argv.size()), argv.data(), err);
            std::cout.rdbuf(previous);

            EXPECT_EQ(code, EXIT_FAILURE);
            EXPECT_NE(err.str().find("metadata-util"), std::string::npos) << err.str();
            EXPECT_NE(err.str().find("DDB_METADATA_UTIL_PATH"), std::string::npos) << err.str();
            EXPECT_EQ(countEntries(root), 0);
        }
    }

    TEST(ExifIndex, aJpgThatIsNotAJpegDoesNotHideAMissingHelper)
    {
        // The helper being gone is not a problem of the file: the read of a ".jpg" that is not an
        // image stops the batch the same way
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        fileWriteAllText(root / "bad.jpg", "this is not a jpeg");
        initIndex(root.string());
        auto db = ddb::open(root.string(), true);

        const EnvVarGuard env(exif::kMetadataUtilEnv, io::toUtf8(ta.getFolder("tools") / "no-such-metadata-util"));
        exif::HelperMetadataReader pool;
        exif::ScopedMetadataReader scope(pool);
        AddOptions options;
        options.stopOnError = false;
        AddResult result;
        EXPECT_THROW(addToIndexEx(db.get(), {(root / "bad.jpg").string()}, options, result), MetadataHelperException);
        EXPECT_EQ(countEntries(root), 0);
    }

    TEST(ExifThermal, withoutTheHelperTheThermalCalibrationIsNotTakenForAFileWithoutExif)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        writeJpeg(root / "photo.jpg");

        // A file the reader has no metadata for gives the default calibration
        {
            CountingReader reader;
            exif::ScopedMetadataReader scope(reader);
            EXPECT_FALSE(extractThermalCalibration(io::toUtf8(root / "photo.jpg")).valid);
            EXPECT_EQ(reader.reads.load(), 1u);
        }

        const EnvVarGuard env(exif::kMetadataUtilEnv, io::toUtf8(ta.getFolder("tools") / "no-such-metadata-util"));
        exif::HelperMetadataReader pool;
        exif::ScopedMetadataReader scope(pool);
        EXPECT_THROW(extractThermalCalibration(io::toUtf8(root / "photo.jpg")), MetadataHelperException);
    }

    TEST(ExifMultispectral, withoutTheHelperTheMergeStops)
    {
        TestArea ta(TEST_NAME, true);
        std::vector<std::string> inputs;
        for (int band = 0; band < 2; ++band)
        {
            testutils::GeoRasterSpec spec;
            spec.width = spec.height = 32;
            spec.bands = 1;
            inputs.push_back(io::toUtf8(testutils::createGeoRaster(ta.getPath("band" + std::to_string(band) + ".tif"), spec)));
        }

        const EnvVarGuard env(exif::kMetadataUtilEnv, io::toUtf8(ta.getFolder("tools") / "no-such-metadata-util"));
        exif::HelperMetadataReader pool;
        exif::ScopedMetadataReader scope(pool);
        EXPECT_THROW(mergeMultispectral(inputs, io::toUtf8(ta.getPath("merged.tif"))), MetadataHelperException);
    }

    // ------------------------------------------------------------------ setexif

    // Runs `ddb setexif <args>` and returns the exit code, with what the command wrote to stderr
    struct SetExifRun
    {
        int code = -1;
        std::string err;
    };

    SetExifRun runSetExif(std::vector<std::string> args)
    {
        cmd::SetExif command;
        std::vector<char *> argv{const_cast<char *>("ddb")};
        for (auto &arg : args)
            argv.push_back(arg.data());

        std::ostringstream captured;
        std::ostringstream ignored;
        std::streambuf *previous = std::cerr.rdbuf(captured.rdbuf());
        SetExifRun run;
        try
        {
            run.code = cmd::execute(command, static_cast<int>(argv.size()), argv.data(), ignored);
        }
        catch (...)
        {
            std::cerr.rdbuf(previous);
            throw;
        }
        std::cerr.rdbuf(previous);
        run.err = captured.str() + ignored.str();
        return run;
    }

    GeoLocation readGeo(const fs::path &file)
    {
        const auto parser = readExifParser(file);
        EXPECT_NE(parser, nullptr) << io::toUtf8(file);
        GeoLocation geo;
        if (parser)
        {
            EXPECT_TRUE(parser->extractGeo(geo)) << io::toUtf8(file);
        }
        return geo;
    }

    TEST(ExifSetExif, gpsAndAltitudeRoundTripThroughTheReader)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path photo = ta.getFolder("ds") / "photo.jpg";
        writeJpeg(photo);
        const std::string path = io::toUtf8(photo);

        auto run = runSetExif({"--gps", "46.0667,11.1167,194", path});
        EXPECT_EQ(run.code, EXIT_SUCCESS) << run.err;
        EXPECT_EQ(run.err.find("Cannot write metadata"), std::string::npos) << run.err;
        GeoLocation geo = readGeo(photo);
        EXPECT_NEAR(geo.latitude, 46.0667, 1e-4);
        EXPECT_NEAR(geo.longitude, 11.1167, 1e-4);
        EXPECT_NEAR(geo.altitude, 194.0, 1e-3);

        run = runSetExif({"--gps-alt", "250.5", path});
        EXPECT_EQ(run.code, EXIT_SUCCESS) << run.err;
        geo = readGeo(photo);
        EXPECT_NEAR(geo.altitude, 250.5, 1e-3);
        EXPECT_NEAR(geo.latitude, 46.0667, 1e-4);
        EXPECT_NEAR(geo.longitude, 11.1167, 1e-4);

        // South and west, an altitude below the sea
        run = runSetExif({"--gps-lat=-33.5", "--gps-lon=-151.25", "--gps-alt=-12", path});
        EXPECT_EQ(run.code, EXIT_SUCCESS) << run.err;
        geo = readGeo(photo);
        EXPECT_NEAR(geo.latitude, -33.5, 1e-4);
        EXPECT_NEAR(geo.longitude, -151.25, 1e-4);
        EXPECT_NEAR(geo.altitude, -12.0, 1e-3);
    }

    TEST(ExifSetExif, aNonAsciiPathIsEdited)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path folder = ta.getFolder("ds") / io::fromUtf8("citt\xC3\xA0_\xC3\xBC");
        fs::create_directories(folder);
        const fs::path photo = folder / io::fromUtf8("\xC3\xA9t\xC3\xA9_\xE6\x97\xA5.jpg");
        writeJpeg(photo);

        const auto run = runSetExif({"--gps", "10.5,20.25,30", io::toUtf8(photo)});
        EXPECT_EQ(run.code, EXIT_SUCCESS) << run.err;
        EXPECT_EQ(run.err.find("Cannot write metadata"), std::string::npos) << run.err;
        const GeoLocation geo = readGeo(photo);
        EXPECT_NEAR(geo.latitude, 10.5, 1e-4);
        EXPECT_NEAR(geo.longitude, 20.25, 1e-4);
        EXPECT_NEAR(geo.altitude, 30.0, 1e-3);
    }

    TEST(ExifSetExif, aFileThatCannotBeEditedStopsTheCommandBeforeAnyWrite)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path photo = ta.getFolder("ds") / "photo.jpg";
        const fs::path text = ta.getFolder("ds") / "notes.txt";
        writeJpeg(photo);
        fileWriteAllText(text, "alpha");

        const auto run = runSetExif({"--gps-lat", "1", io::toUtf8(photo), io::toUtf8(text), io::toUtf8(ta.getPath("missing.jpg"))});
        EXPECT_EQ(run.code, EXIT_FAILURE);
        EXPECT_NE(run.err.find("notes.txt"), std::string::npos) << run.err;
        EXPECT_NE(run.err.find("missing.jpg: does not exist"), std::string::npos) << run.err;
        // The editable file was not touched
        const auto parser = readExifParser(photo);
        ASSERT_NE(parser, nullptr);
        GeoLocation geo;
        EXPECT_FALSE(parser->extractGeo(geo));
    }

    // A reader of the test: reads every file as an image, fails the writes of the files it is told to
    class ScriptedWriter : public exif::IMetadataReader
    {
      public:
        exif::ReadResult read(const fs::path &) override
        {
            exif::ReadResult result;
            result.metadata = exif::ImageMetadata(8, 8, {}, {});
            return result;
        }
        exif::WriteResult write(const fs::path &file, const exif::TagAssignments &set) override
        {
            requests.emplace_back(file.filename().string(), set);
            exif::WriteResult result;
            result.ok = failing.count(file.filename().string()) == 0;
            if (!result.ok)
                result.message = "disk is full";
            return result;
        }

        std::set<std::string> failing;
        std::vector<std::pair<std::string, exif::TagAssignments>> requests;
    };

    TEST(ExifSetExif, aFileThatCannotBeWrittenIsReportedAndTheNextOneIsWritten)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        for (const char *name : {"first.jpg", "second.jpg", "third.jpg"})
            fileWriteAllText(root / name, "x");

        ScriptedWriter reader;
        reader.failing = {"first.jpg"};
        exif::ScopedMetadataReader scope(reader);

        const auto run = runSetExif({"--gps", "46.5,-11.25,-10.5", io::toUtf8(root / "first.jpg"),
                                     io::toUtf8(root / "second.jpg"), io::toUtf8(root / "third.jpg")});

        // The library prints nothing: the command does, once for the file, goes on, and then
        // fails (stderr holds the log of the test run too)
        const std::string failed = "Cannot write metadata to " + io::toUtf8(root / "first.jpg") + ": disk is full\n";
        EXPECT_NE(run.err.find(failed), std::string::npos) << run.err;
        EXPECT_EQ(run.err.find("Cannot write metadata"), run.err.rfind("Cannot write metadata")) << run.err;
        EXPECT_EQ(run.code, EXIT_FAILURE);
        ASSERT_EQ(reader.requests.size(), 3u);
        EXPECT_EQ(reader.requests[0].first, "first.jpg");
        EXPECT_EQ(reader.requests[1].first, "second.jpg");
        EXPECT_EQ(reader.requests[2].first, "third.jpg");

        // One request per file, with the tags in the order the editor sets them
        const exif::TagAssignments expected = {{"Exif.GPSInfo.GPSAltitude", "10500/1000"},
                                               {"Exif.GPSInfo.GPSAltitudeRef", "1"},
                                               {"Exif.GPSInfo.GPSLatitude", "46/1 30/1 0/10000"},
                                               {"Exif.GPSInfo.GPSLatitudeRef", "N"},
                                               {"Exif.GPSInfo.GPSLongitude", "11/1 15/1 0/10000"},
                                               {"Exif.GPSInfo.GPSLongitudeRef", "W"}};
        for (const auto &request : reader.requests)
            EXPECT_EQ(request.second, expected) << request.first;
    }

    // A failed file fails the command only once every option has been applied to every file;
    // with every write succeeding the same command succeeds
    TEST(ExifSetExif, aFailedWriteFailsTheCommandAfterEveryFileIsWritten)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        for (const char *name : {"first.jpg", "second.jpg", "third.jpg"})
            fileWriteAllText(root / name, "x");
        const std::vector<std::string> args = {"--gps-alt", "12", "--gps-lat", "45.5", io::toUtf8(root / "first.jpg"),
                                               io::toUtf8(root / "second.jpg"), io::toUtf8(root / "third.jpg")};

        ScriptedWriter reader;
        reader.failing = {"second.jpg"};
        {
            exif::ScopedMetadataReader scope(reader);
            const auto run = runSetExif(args);
            EXPECT_EQ(run.code, EXIT_FAILURE) << run.err;
            const std::string failed = "Cannot write metadata to " + io::toUtf8(root / "second.jpg") + ": disk is full\n";
            const size_t first = run.err.find(failed);
            ASSERT_NE(first, std::string::npos) << run.err;
            // Once for the altitude, once for the latitude
            EXPECT_NE(run.err.find(failed, first + failed.size()), std::string::npos) << run.err;
            for (const char *written : {"first.jpg", "third.jpg"})
                EXPECT_EQ(run.err.find("Cannot write metadata to " + io::toUtf8(root / written)), std::string::npos)
                    << run.err;
        }
        const std::vector<std::string> order = {"first.jpg", "second.jpg", "third.jpg"};
        ASSERT_EQ(reader.requests.size(), 6u);
        for (size_t i = 0; i < reader.requests.size(); ++i)
            EXPECT_EQ(reader.requests[i].first, order[i % 3]) << i;

        ScriptedWriter healthy;
        exif::ScopedMetadataReader scope(healthy);
        const auto run = runSetExif(args);
        EXPECT_EQ(run.code, EXIT_SUCCESS) << run.err;
        EXPECT_EQ(healthy.requests.size(), 6u);
    }

    TEST(ExifSetExif, theEditorReturnsTheFailuresAndNeverPrints)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        fileWriteAllText(root / "a.jpg", "x");
        fileWriteAllText(root / "b.jpg", "x");

        ScriptedWriter reader;
        reader.failing = {"a.jpg", "b.jpg"};
        exif::ScopedMetadataReader scope(reader);

        std::ostringstream captured;
        std::streambuf *previous = std::cerr.rdbuf(captured.rdbuf());
        std::vector<std::string> files = {io::toUtf8(root / "a.jpg"), io::toUtf8(root / "b.jpg")};
        const auto failures = ExifEditor(files).SetGPSAltitude(5.0);
        std::cerr.rdbuf(previous);

        ASSERT_EQ(failures.size(), 2u);
        EXPECT_EQ(failures[0].file, io::toUtf8(root / "a.jpg"));
        EXPECT_EQ(failures[1].file, io::toUtf8(root / "b.jpg"));
        EXPECT_EQ(failures[0].message, "disk is full");
        EXPECT_EQ(captured.str().find("Cannot write metadata"), std::string::npos) << captured.str();
        // Both files were tried
        EXPECT_EQ(reader.requests.size(), 2u);
    }

    TEST(ExifSetExif, aFileThatDoesNotExistThrows)
    {
        TestArea ta(TEST_NAME, true);
        ScriptedWriter reader;
        exif::ScopedMetadataReader scope(reader);
        EXPECT_THROW((void)ExifEditor(io::toUtf8(ta.getPath("missing.jpg"))).SetGPS(1, 2, 3), FSException);
        EXPECT_TRUE(reader.requests.empty());
    }

    TEST(ExifSetExif, canEditFollowsWhatTheReaderCanRead)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        writeJpeg(root / "photo.jpg");
        fileWriteAllText(root / "bad.jpg", "not a jpeg");

        std::ostringstream rejected;
        std::vector<std::string> files = {io::toUtf8(root / "photo.jpg")};
        EXPECT_TRUE(ExifEditor(files).canEdit(rejected)) << rejected.str();
        EXPECT_EQ(rejected.str(), "");

        files = {io::toUtf8(root / "photo.jpg"), io::toUtf8(root / "bad.jpg"), io::toUtf8(root),
                 io::toUtf8(root / "missing.jpg")};
        EXPECT_FALSE(ExifEditor(files).canEdit(rejected));
        const std::string text = rejected.str();
        EXPECT_EQ(text.find("photo.jpg"), std::string::npos) << text;
        EXPECT_NE(text.find(io::toUtf8(root / "bad.jpg") + ": "), std::string::npos) << text;
        EXPECT_NE(text.find(io::toUtf8(root) + ": cannot set EXIFs to a directory"), std::string::npos) << text;
        EXPECT_NE(text.find("missing.jpg: does not exist"), std::string::npos) << text;
    }
}
