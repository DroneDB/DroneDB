/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DDB-04 — applyDelta() moves the added files instead of copying them, indexes them with
// the hashes of the delta in one transaction, and removes entries in one transaction.
//
//  - a file is renamed into the dataset unless it has other hard links (computeDeltaLocals
//    links local copies into the source folder): those are copied, never shared;
//  - with verifyHashes the added files are checked before anything changes; without it the
//    files are not read again and the index stores the delta's hashes;
//  - conflict handling (DontMerge / KeepOurs / KeepTheirs) is unchanged.

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <sstream>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "gtest/gtest.h"

#include "dbops.h"
#include "ddb.h"
#include "delta.h"
#include "exceptions.h"
#include "hash.h"
#include "mio.h"
#include "delta_apply.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    std::string readFile(const fs::path &p)
    {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

#ifndef _WIN32
    ino_t inode(const fs::path &p)
    {
        struct stat st {};
        EXPECT_EQ(stat(p.string().c_str(), &st), 0) << p;
        return st.st_ino;
    }
#endif

    // Calls `onLine` for every line written to it
    class LineHook : public std::streambuf
    {
        std::string line;
        std::function<void(const std::string &)> onLine;

    protected:
        int overflow(int c) override
        {
            if (c == '\n')
            {
                onLine(line);
                line.clear();
            }
            else if (c != EOF)
                line += static_cast<char>(c);
            return c;
        }

    public:
        explicit LineHook(std::function<void(const std::string &)> onLine) : onLine(std::move(onLine)) {}
    };

    struct Fixture : IndexFixture
    {
        fs::path source; // like the Registry push folder: inside .ddb/tmp, same volume

        explicit Fixture(const std::string &name) : IndexFixture(name)
        {
            source = root / ".ddb" / "tmp" / "push-test" / "add";
            fs::create_directories(source);

            writeFile(root / "keep.txt", "keep");
            writeFile(root / "old.txt", "old");
            writeFile(root / "dir" / "gone.txt", "gone");
            addToIndex(db.get(), {(root / "keep.txt").string(), (root / "old.txt").string(),
                                  (root / "dir").string()});
        }

        // A file the remote side adds, staged in the source folder
        AddAction stage(const std::string &rel, const std::string &content) const
        {
            writeFile(source / rel, content);
            return AddAction(rel, Hash::fileSHA256((source / rel).string()));
        }

        std::vector<Conflict> apply(const Delta &d, MergeStrategy strategy = MergeStrategy::KeepTheirs,
                                    bool verify = true, std::ostream &out = std::cout)
        {
            DeltaApplication application;
            application.sourcePath = source;
            application.mergeStrategy = strategy;
            application.verifyHashes = verify;
            return applyDelta(d, db.get(), application, out);
        }

        // applyDelta for a caller that verified `verified` itself (XI-05)
        std::vector<Conflict> applyVerified(const Delta &d, const VerifiedHashes &verified, bool verify = true)
        {
            std::ostringstream out;
            DeltaApplication application;
            application.sourcePath = source;
            application.mergeStrategy = MergeStrategy::KeepTheirs;
            application.verifyHashes = verify;
            application.verifiedHashes = verified;
            return applyDelta(d, db.get(), application, out);
        }

        // DDBApplyDeltaEx on this dataset (the fixture's connection is closed first); the
        // conflicts (JSON text) go to *conflicts when it succeeds
        DDBErr applyEx(const Delta &d, const char *verifiedHashesJson, std::string *conflicts = nullptr)
        {
            db->close();
            const auto deltaJson = json(d).dump();
            char *out = nullptr;
            const DDBErr err = DDBApplyDeltaEx(deltaJson.c_str(), source.string().c_str(), root.string().c_str(),
                                               MergeStrategy::KeepTheirs, "[]", verifiedHashesJson, &out);
            if (out != nullptr && conflicts != nullptr)
                *conflicts = out;
            DDBFree(out);
            db = ddb::open(root.string(), false);
            return err;
        }
    };

#ifdef __linux__
    // Bytes this process read with read(2) and friends so far, all threads (/proc/self/io
    // rchar): page-cache hits included, so it counts what a cold cache reads from the disk
    unsigned long long bytesReadByProcess()
    {
        std::ifstream io("/proc/self/io");
        std::string key;
        unsigned long long value = 0;
        while (io >> key >> value)
            if (key == "rchar:")
                return value;
        ADD_FAILURE() << "no rchar in /proc/self/io";
        return 0;
    }
#endif

    TEST(DDB04_ApplyDelta, addsRemovesAndUpdates)
    {
        Fixture f(TEST_NAME);
        Delta d;
        d.adds.push_back(AddAction("new", ""));
        d.adds.push_back(f.stage("new/a.txt", "alpha"));
        d.adds.push_back(f.stage("old.txt", "old, modified"));
        d.removes.push_back(RemoveAction("dir/gone.txt", hashOf(f.db.get(), "dir/gone.txt")));
        d.removes.push_back(RemoveAction("dir", ""));

        std::ostringstream out;
        const auto conflicts = f.apply(d, MergeStrategy::KeepTheirs, true, out);
        EXPECT_TRUE(conflicts.empty());

        EXPECT_EQ(readFile(f.root / "new" / "a.txt"), "alpha");
        EXPECT_EQ(readFile(f.root / "old.txt"), "old, modified");
        EXPECT_FALSE(fs::exists(f.root / "dir"));

        EXPECT_EQ(hashOf(f.db.get(), "new"), "");
        EXPECT_EQ(hashOf(f.db.get(), "new/a.txt"), Hash::fileSHA256((f.root / "new" / "a.txt").string()));
        EXPECT_EQ(hashOf(f.db.get(), "old.txt"), Hash::fileSHA256((f.root / "old.txt").string()));
        EXPECT_EQ(hashOf(f.db.get(), "dir"), "<missing>");
        EXPECT_EQ(hashOf(f.db.get(), "dir/gone.txt"), "<missing>");
        EXPECT_NE(hashOf(f.db.get(), "keep.txt"), "<missing>");

        const auto log = out.str();
        EXPECT_NE(log.find("D\tdir/gone.txt\n"), std::string::npos) << log;
        EXPECT_NE(log.find("D\tdir\n"), std::string::npos) << log;
        EXPECT_NE(log.find("A\tnew/a.txt\n"), std::string::npos) << log;
        EXPECT_NE(log.find("U\told.txt\n"), std::string::npos) << log;
    }

    TEST(DDB04_ApplyDelta, movesFilesWhenSameVolume)
    {
        Fixture f(TEST_NAME);
        Delta d;
        d.adds.push_back(f.stage("photo.jpg", std::string(256 * 1024, 'x')));
#ifndef _WIN32
        const auto sourceInode = inode(f.source / "photo.jpg");
#endif

        f.apply(d);

        EXPECT_FALSE(fs::exists(f.source / "photo.jpg"));
        EXPECT_EQ(readFile(f.root / "photo.jpg"), std::string(256 * 1024, 'x'));
#ifndef _WIN32
        EXPECT_EQ(inode(f.root / "photo.jpg"), sourceInode) << "the file was copied, not renamed";
#endif
    }

    TEST(DDB04_ApplyDelta, hardLinkedLocalFileIsCopied)
    {
        // A local file with the hash of an add is hard-linked into the source folder
        // (computeDeltaLocals): it must not end up sharing its data with the original.
        Fixture f(TEST_NAME);
        Delta d;
        d.adds.push_back(AddAction("copy-of-keep.txt", hashOf(f.db.get(), "keep.txt")));
        computeDeltaLocals(d, f.db.get(), f.source.string());
        ASSERT_TRUE(fs::exists(f.source / "copy-of-keep.txt"));

        f.apply(d);

        EXPECT_EQ(readFile(f.root / "copy-of-keep.txt"), "keep");
        EXPECT_EQ(readFile(f.root / "keep.txt"), "keep");
        EXPECT_EQ(fs::hard_link_count(f.root / "keep.txt"), 1u);
        EXPECT_EQ(fs::hard_link_count(f.root / "copy-of-keep.txt"), 1u);
        EXPECT_EQ(hashOf(f.db.get(), "copy-of-keep.txt"), hashOf(f.db.get(), "keep.txt"));
    }

    TEST(DDB04_ApplyDelta, doesNotRehashWithoutVerification)
    {
        // The index stores the delta's hash: the file is not read again.
        Fixture f(TEST_NAME);
        writeFile(f.source / "a.txt", "alpha");
        Delta d;
        d.adds.push_back(AddAction("a.txt", "declared-hash"));

        f.apply(d, MergeStrategy::KeepTheirs, false);

        EXPECT_EQ(hashOf(f.db.get(), "a.txt"), "declared-hash");
    }

    TEST(DDB04_ApplyDelta, verificationRejectsMismatchBeforeAnyChange)
    {
        Fixture f(TEST_NAME);
        Delta d;
        d.adds.push_back(f.stage("a.txt", "alpha"));
        writeFile(f.source / "b.txt", "bravo");
        d.adds.push_back(AddAction("b.txt", "not-the-hash-of-bravo"));
        d.removes.push_back(RemoveAction("keep.txt", hashOf(f.db.get(), "keep.txt")));

        EXPECT_THROW(f.apply(d), AppException);

        // Nothing changed: no add moved or indexed, no remove applied
        EXPECT_TRUE(fs::exists(f.source / "a.txt"));
        EXPECT_FALSE(fs::exists(f.root / "a.txt"));
        EXPECT_EQ(hashOf(f.db.get(), "a.txt"), "<missing>");
        EXPECT_TRUE(fs::exists(f.root / "keep.txt"));
        EXPECT_NE(hashOf(f.db.get(), "keep.txt"), "<missing>");
    }

    TEST(DDB04_ApplyDelta, addsInOneTransaction)
    {
        Fixture f(TEST_NAME);
        Delta d;
        for (int i = 0; i < 20; i++)
            d.adds.push_back(f.stage("f" + std::to_string(i) + ".txt", "content " + std::to_string(i)));

        // While applyDelta reports the adds, another connection must not see any of them
        SqliteDatabase reader;
        reader.open((f.root / DDB_FOLDER / DDB_DATABASE_FILE).string());
        int reported = 0, visibleWhileReporting = 0;
        LineHook hook([&](const std::string &line)
                      {
                          if (line.rfind("A\t", 0) != 0) return;
                          reported++;
                          auto q = reader.query("SELECT COUNT(*) FROM entries WHERE path LIKE 'f%.txt'");
                          q->fetch();
                          visibleWhileReporting = std::max(visibleWhileReporting, q->getInt(0)); });
        std::ostream out(&hook);

        f.apply(d, MergeStrategy::KeepTheirs, true, out);

        EXPECT_EQ(reported, 20);
        EXPECT_EQ(visibleWhileReporting, 0) << "adds were committed one by one";
        auto q = reader.query("SELECT COUNT(*) FROM entries WHERE path LIKE 'f%.txt'");
        q->fetch();
        EXPECT_EQ(q->getInt(0), 20);
    }

    TEST(DDB04_ApplyDelta, indexesOnlyDeltaPaths)
    {
        // Adding a folder indexes the folder, not whatever else sits in it on disk
        Fixture f(TEST_NAME);
        writeFile(f.root / "new" / "unrelated.txt", "not part of the push");
        Delta d;
        d.adds.push_back(AddAction("new", ""));
        d.adds.push_back(f.stage("new/a.txt", "alpha"));

        f.apply(d);

        EXPECT_EQ(hashOf(f.db.get(), "new"), "");
        EXPECT_NE(hashOf(f.db.get(), "new/a.txt"), "<missing>");
        EXPECT_EQ(hashOf(f.db.get(), "new/unrelated.txt"), "<missing>");
    }

    TEST(DDB04_IndexPathList, eachFileOnceAndOptionalRecursion)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        writeFile(root / "f" / "a.txt", "a");
        writeFile(root / "f" / "g" / "b.txt", "b");

        const auto count = [](const std::vector<fs::path> &list, const fs::path &p)
        { return std::count(list.begin(), list.end(), p); };

        // Listed explicitly and found again under its folder
        auto list = getIndexPathList(root, {(root / "f").string(), (root / "f" / "a.txt").string()}, true);
        EXPECT_EQ(count(list, root / "f" / "a.txt"), 1);
        EXPECT_EQ(count(list, root / "f" / "g" / "b.txt"), 1);
        EXPECT_EQ(list.size(), 4u); // f, f/g, f/a.txt, f/g/b.txt

        list = getIndexPathList(root, {(root / "f").string(), (root / "f" / "a.txt").string()}, true, false);
        EXPECT_EQ(count(list, root / "f"), 1);
        EXPECT_EQ(count(list, root / "f" / "a.txt"), 1);
        EXPECT_EQ(list.size(), 2u);
    }

    TEST(DDB04_ApplyDelta, unsortedRemoves)
    {
        // A hand-made delta may list a folder before its content
        Fixture f(TEST_NAME);
        Delta d;
        d.removes.push_back(RemoveAction("dir", ""));
        d.removes.push_back(RemoveAction("dir/gone.txt", hashOf(f.db.get(), "dir/gone.txt")));

        EXPECT_TRUE(f.apply(d).empty());
        EXPECT_FALSE(fs::exists(f.root / "dir"));
        EXPECT_EQ(hashOf(f.db.get(), "dir"), "<missing>");
        EXPECT_EQ(hashOf(f.db.get(), "dir/gone.txt"), "<missing>");
    }

    TEST(DDB04_ApplyDelta, typeFlipFolderToFile)
    {
        Fixture f(TEST_NAME);
        Delta d;
        d.removes.push_back(RemoveAction("dir/gone.txt", hashOf(f.db.get(), "dir/gone.txt")));
        d.removes.push_back(RemoveAction("dir", ""));
        d.adds.push_back(f.stage("dir", "now a file"));

        const auto conflicts = f.apply(d, MergeStrategy::DontMerge);

        EXPECT_TRUE(conflicts.empty());
        EXPECT_TRUE(fs::is_regular_file(f.root / "dir"));
        EXPECT_EQ(hashOf(f.db.get(), "dir"), Hash::fileSHA256((f.root / "dir").string()));
        EXPECT_EQ(hashOf(f.db.get(), "dir/gone.txt"), "<missing>");
    }

    TEST(DDB04_ApplyDelta, conflicts)
    {
        // The remote side changed old.txt and removed keep.txt, but its base hashes differ
        // from ours (both were modified locally)
        const auto makeDelta = [](Fixture &f)
        {
            Delta d;
            d.adds.push_back(f.stage("old.txt", "theirs"));
            d.removes.push_back(RemoveAction("keep.txt", "remote-base-hash"));
            return d;
        };

        {
            Fixture f(TEST_NAME + "-dontmerge");
            const auto ours = hashOf(f.db.get(), "old.txt");
            const auto conflicts = f.apply(makeDelta(f), MergeStrategy::DontMerge);
            ASSERT_EQ(conflicts.size(), 2u);
            EXPECT_EQ(conflicts[0].path, "keep.txt");
            EXPECT_EQ(conflicts[0].type, ConflictType::RemoteDeleteLocalModified);
            EXPECT_EQ(conflicts[1].path, "old.txt");
            EXPECT_EQ(conflicts[1].type, ConflictType::BothModified);
            EXPECT_EQ(readFile(f.root / "old.txt"), "old");
            EXPECT_EQ(hashOf(f.db.get(), "old.txt"), ours);
            EXPECT_TRUE(fs::exists(f.root / "keep.txt"));
        }
        {
            Fixture f(TEST_NAME + "-keepours");
            const auto conflicts = f.apply(makeDelta(f), MergeStrategy::KeepOurs);
            EXPECT_TRUE(conflicts.empty());
            EXPECT_EQ(readFile(f.root / "old.txt"), "old");
            EXPECT_TRUE(fs::exists(f.root / "keep.txt"));
        }
        {
            Fixture f(TEST_NAME + "-keeptheirs");
            const auto conflicts = f.apply(makeDelta(f), MergeStrategy::KeepTheirs);
            EXPECT_TRUE(conflicts.empty());
            EXPECT_EQ(readFile(f.root / "old.txt"), "theirs");
            EXPECT_EQ(hashOf(f.db.get(), "old.txt"), Hash::fileSHA256((f.root / "old.txt").string()));
            EXPECT_FALSE(fs::exists(f.root / "keep.txt"));
            EXPECT_EQ(hashOf(f.db.get(), "keep.txt"), "<missing>");
        }
    }

    TEST(ApplyDelta, remoteChangeOfAnUnchangedLocalFileIsNoConflict)
    {
        // The delta of a pull is computed against the last synced stamp: a file modified on
        // the remote and unchanged locally since then is no conflict
        Fixture f(TEST_NAME);
        const auto base = hashOf(f.db.get(), "old.txt");
        auto add = f.stage("old.txt", "remote edit");
        add.baseHash = base;
        Delta d;
        d.adds.push_back(add);

        EXPECT_TRUE(f.apply(d, MergeStrategy::DontMerge).empty());
        EXPECT_EQ(readFile(f.root / "old.txt"), "remote edit");
        EXPECT_EQ(hashOf(f.db.get(), "old.txt"), add.hash);
    }

    TEST(ApplyDelta, localChangeSinceTheBaseIsAConflict)
    {
        Fixture f(TEST_NAME);
        auto add = f.stage("old.txt", "remote edit");
        add.baseHash = "hash-at-last-sync";
        Delta d;
        d.adds.push_back(add);

        const auto conflicts = f.apply(d, MergeStrategy::DontMerge);
        ASSERT_EQ(conflicts.size(), 1u);
        EXPECT_EQ(conflicts[0].type, ConflictType::BothModified);
        EXPECT_EQ(readFile(f.root / "old.txt"), "old");
    }

    TEST(ApplyDelta, rootEntryIsNeverRemoved)
    {
        // A stamp from an older version may list the root as "": removing it must not
        // delete the dataset folder
        Fixture f(TEST_NAME);
        Delta d;
        d.removes.push_back(RemoveAction("", ""));
        d.adds.push_back(AddAction("", ""));

        EXPECT_TRUE(f.apply(d).empty());
        EXPECT_TRUE(fs::exists(f.root / "keep.txt"));
        EXPECT_TRUE(fs::exists(f.root / DDB_FOLDER));
        EXPECT_EQ(hashOf(f.db.get(), ""), "<missing>");
    }

    TEST(ApplyDelta, pathsOutsideTheDatasetAreRejected)
    {
        Fixture f(TEST_NAME);
        for (const std::string bad : {"../outside.txt", "dir/../../outside.txt", "/etc/passwd"})
        {
            Delta d;
            d.removes.push_back(RemoveAction("keep.txt", hashOf(f.db.get(), "keep.txt")));
            d.removes.push_back(RemoveAction(bad, "h"));
            EXPECT_THROW(f.apply(d, MergeStrategy::KeepTheirs, false), InvalidArgsException) << bad;
            EXPECT_TRUE(fs::exists(f.root / "keep.txt")) << "nothing changes";

            Delta a;
            a.adds.push_back(AddAction(bad, "h"));
            EXPECT_THROW(f.apply(a, MergeStrategy::KeepTheirs, false), InvalidArgsException) << bad;
        }
    }

    TEST(DDB04_ApplyDelta, cApiVerifiesHashes)
    {
        Fixture f(TEST_NAME);
        writeFile(f.source / "a.txt", "alpha");
        Delta d;
        d.adds.push_back(AddAction("a.txt", "not-the-hash"));
        const auto deltaJson = json(d).dump();
        const auto root = f.root.string();
        const auto source = f.source.string();
        f.db->close();

        char *conflicts = nullptr;
        EXPECT_NE(DDBApplyDelta(deltaJson.c_str(), source.c_str(), root.c_str(), MergeStrategy::KeepTheirs, "[]", &conflicts),
                  DDBERR_NONE);
        EXPECT_NE(std::string(DDBGetLastError()).find("Hash mismatch for a.txt"), std::string::npos)
            << DDBGetLastError();
        EXPECT_TRUE(fs::exists(f.source / "a.txt"));
    }

    // ------------------------------------------------------------------ XI-05: verified hashes

    TEST(XI05_ApplyDelta, verifiedFilesAreNotReadAgain)
    {
        // b.txt does not have the hash the delta declares: trusted, it is indexed with the
        // delta's hash, so its bytes were never read
        Fixture f(TEST_NAME);
        Delta d;
        d.adds.push_back(f.stage("a.txt", "alpha"));
        writeFile(f.source / "b.txt", "not what the caller verified");
        const auto declared = Hash::strSHA256("bravo");
        d.adds.push_back(AddAction("b.txt", declared));

        EXPECT_TRUE(f.applyVerified(d, {{"b.txt", declared}}).empty());

        EXPECT_EQ(hashOf(f.db.get(), "a.txt"), d.adds[0].hash);
        EXPECT_EQ(hashOf(f.db.get(), "b.txt"), declared);
    }

    TEST(XI05_ApplyDelta, filesNotListedAreStillHashed)
    {
        Fixture f(TEST_NAME);
        Delta d;
        d.adds.push_back(f.stage("a.txt", "alpha"));
        writeFile(f.source / "b.txt", "bravo");
        d.adds.push_back(AddAction("b.txt", Hash::strSHA256("not bravo")));

        try
        {
            f.applyVerified(d, {{"a.txt", d.adds[0].hash}});
            FAIL() << "the unlisted b.txt was not hashed";
        }
        catch (const AppException &e)
        {
            EXPECT_NE(std::string(e.what()).find("Hash mismatch for b.txt"), std::string::npos) << e.what();
        }
        EXPECT_TRUE(fs::exists(f.source / "a.txt")) << "nothing changes";
        EXPECT_EQ(hashOf(f.db.get(), "a.txt"), "<missing>");
    }

    TEST(XI05_ApplyDelta, verifiedHashMustBeTheDeltaHash)
    {
        // The caller verified other bytes than the delta declares: rejected before any change,
        // with or without the hash check of the other files
        for (const bool verify : {true, false})
        {
            Fixture f(TEST_NAME + (verify ? "-verify" : "-noverify"));
            Delta d;
            d.adds.push_back(f.stage("a.txt", "alpha"));
            d.removes.push_back(RemoveAction("keep.txt", hashOf(f.db.get(), "keep.txt")));

            try
            {
                f.applyVerified(d, {{"a.txt", Hash::strSHA256("something else")}}, verify);
                FAIL() << "a different verified hash was trusted";
            }
            catch (const InvalidArgsException &e)
            {
                EXPECT_NE(std::string(e.what()).find("Verified hash mismatch for a.txt"), std::string::npos)
                    << e.what();
            }
            EXPECT_TRUE(fs::exists(f.source / "a.txt"));
            EXPECT_EQ(hashOf(f.db.get(), "a.txt"), "<missing>");
            EXPECT_TRUE(fs::exists(f.root / "keep.txt"));
            EXPECT_NE(hashOf(f.db.get(), "keep.txt"), "<missing>");
        }
    }

    TEST(XI05_ApplyDelta, verifiedHashOfAPathTheDeltaDoesNotAddIsRejected)
    {
        Fixture f(TEST_NAME);
        Delta d;
        d.adds.push_back(AddAction("new", ""));
        d.adds.push_back(f.stage("new/a.txt", "alpha"));
        const auto h = d.adds[1].hash;

        // Another file, a folder add (no hash), a path of the delta spelled differently
        for (const VerifiedHashes &bad : {VerifiedHashes{{"other.txt", h}}, VerifiedHashes{{"new", ""}},
                                          VerifiedHashes{{"new\\a.txt", h}}})
        {
            EXPECT_THROW(f.applyVerified(d, bad), InvalidArgsException) << bad.begin()->first;
            EXPECT_THROW(f.applyVerified(d, bad, false), InvalidArgsException) << bad.begin()->first;
            EXPECT_TRUE(fs::exists(f.source / "new" / "a.txt"));
        }

        // Also for a delta without file operations
        EXPECT_THROW(f.applyVerified(Delta(), {{"new/a.txt", h}}), InvalidArgsException);
    }

    TEST(XI05_ApplyDeltaEx, nullVerifiedHashesIsDDBApplyDelta)
    {
        Fixture f(TEST_NAME);
        writeFile(f.source / "a.txt", "alpha");
        Delta d;
        d.adds.push_back(AddAction("a.txt", Hash::strSHA256("not alpha")));

        // Every file hashed: the same failure as DDBApplyDelta
        EXPECT_EQ(f.applyEx(d, nullptr), DDBERR_EXCEPTION);
        EXPECT_NE(std::string(DDBGetLastError()).find("Hash mismatch for a.txt"), std::string::npos)
            << DDBGetLastError();
        EXPECT_TRUE(fs::exists(f.source / "a.txt"));

        // DDB_VERIFY_PUSH_HASHES=0 still skips the check when no hashes are given
        EnvVarGuard noVerify("DDB_VERIFY_PUSH_HASHES", "0");
        std::string conflicts;
        EXPECT_EQ(f.applyEx(d, nullptr, &conflicts), DDBERR_NONE) << DDBGetLastError();
        EXPECT_EQ(conflicts, "[]");
        EXPECT_EQ(hashOf(f.db.get(), "a.txt"), d.adds[0].hash);
    }

    TEST(XI05_ApplyDeltaEx, explicitHashesIgnoreTheEnvironment)
    {
        // An empty object: the caller verified nothing, every file is hashed even with
        // DDB_VERIFY_PUSH_HASHES=0
        Fixture f(TEST_NAME);
        EnvVarGuard noVerify("DDB_VERIFY_PUSH_HASHES", "0");
        writeFile(f.source / "a.txt", "alpha");
        Delta d;
        d.adds.push_back(AddAction("a.txt", Hash::strSHA256("not alpha")));

        EXPECT_EQ(f.applyEx(d, "{}"), DDBERR_EXCEPTION);
        EXPECT_NE(std::string(DDBGetLastError()).find("Hash mismatch for a.txt"), std::string::npos)
            << DDBGetLastError();
        EXPECT_TRUE(fs::exists(f.source / "a.txt"));
    }

    TEST(XI05_ApplyDeltaEx, trustsTheVerifiedFiles)
    {
        Fixture f(TEST_NAME);
        Delta d;
        d.adds.push_back(f.stage("a.txt", "alpha"));
        writeFile(f.source / "b.txt", "bytes nobody reads");
        d.adds.push_back(AddAction("b.txt", Hash::strSHA256("bravo")));
        const json verified = {{"b.txt", d.adds[1].hash}};

        std::string conflicts;
        EXPECT_EQ(f.applyEx(d, verified.dump().c_str(), &conflicts), DDBERR_NONE) << DDBGetLastError();
        EXPECT_EQ(conflicts, "[]");
        EXPECT_EQ(hashOf(f.db.get(), "a.txt"), d.adds[0].hash);
        EXPECT_EQ(hashOf(f.db.get(), "b.txt"), d.adds[1].hash);
        EXPECT_FALSE(fs::exists(f.source / "b.txt"));
    }

    TEST(XI05_ApplyDeltaEx, invalidVerifiedHashesAreInvalidArgs)
    {
        Fixture f(TEST_NAME);
        Delta d;
        d.adds.push_back(f.stage("a.txt", "alpha"));
        const auto h = d.adds[0].hash;
        std::string upper = h;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });

        const std::vector<std::string> bad = {
            "not json",
            "[\"a.txt\"]",
            json({{"a.txt", 1}}).dump(),
            json({{"a.txt", nullptr}}).dump(),
            json({{"a.txt", Hash::strSHA256("other")}}).dump(), // not the delta's hash
            json({{"a.txt", upper}}).dump(),                     // exactly the delta's hash only
            json({{"a.txt", h}, {"b.txt", h}}).dump(),           // b.txt is not in the delta
        };
        for (const auto &v : bad)
        {
            EXPECT_EQ(f.applyEx(d, v.c_str()), DDBERR_INVALIDARGS) << v << ": " << DDBGetLastError();
            EXPECT_TRUE(fs::exists(f.source / "a.txt")) << v;
            EXPECT_EQ(hashOf(f.db.get(), "a.txt"), "<missing>") << v;
        }

        // The other arguments: as DDBApplyDelta, with the 2.0 code
        const auto deltaJson = json(d).dump();
        const auto root = f.root.string();
        const auto source = f.source.string();
        const auto verified = json({{"a.txt", h}}).dump();
        char *out = nullptr;
        EXPECT_EQ(DDBApplyDeltaEx(deltaJson.c_str(), source.c_str(), root.c_str(), MergeStrategy::KeepTheirs, "[]",
                                  verified.c_str(), nullptr),
                  DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBApplyDeltaEx(nullptr, source.c_str(), root.c_str(), MergeStrategy::KeepTheirs, "[]",
                                  verified.c_str(), &out),
                  DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBApplyDeltaEx(deltaJson.c_str(), source.c_str(), root.c_str(), -1, "[]", verified.c_str(), &out),
                  DDBERR_INVALIDARGS);
        EXPECT_EQ(out, nullptr);
        EXPECT_TRUE(fs::exists(f.source / "a.txt"));
    }

#ifdef __linux__
    TEST(XI05_ApplyDeltaEx, pushReadsEachFileOnce)
    {
        // A push of N files: DDBApplyDelta reads every added byte again to hash it, the
        // caller that hashed the stream passes the hashes and libddb reads none of them
        constexpr int kFiles = 16;
        constexpr size_t kFileSize = 1024 * 1024;
        constexpr unsigned long long kPushBytes = static_cast<unsigned long long>(kFiles) * kFileSize;

        const auto stagePush = [&](Fixture &f, Delta &d, json &verified)
        {
            d.adds.push_back(AddAction("push", ""));
            for (int i = 0; i < kFiles; i++)
            {
                const auto add = f.stage("push/f" + std::to_string(i) + ".bin",
                                         std::string(kFileSize, static_cast<char>('a' + i)));
                d.adds.push_back(add);
                verified[add.path] = add.hash;
            }
        };
        const auto measure = [](Fixture &f, const Delta &d, const char *verifiedHashesJson)
        {
            const auto before = bytesReadByProcess();
            EXPECT_EQ(f.applyEx(d, verifiedHashesJson), DDBERR_NONE) << DDBGetLastError();
            return bytesReadByProcess() - before;
        };

        Fixture hashed(TEST_NAME + "-hashed");
        Delta hashedDelta;
        json unused = json::object();
        stagePush(hashed, hashedDelta, unused);
        const auto readHashing = measure(hashed, hashedDelta, nullptr);

        Fixture trusted(TEST_NAME + "-verified");
        Delta trustedDelta;
        json verified = json::object();
        stagePush(trusted, trustedDelta, verified);
        const auto readVerified = measure(trusted, trustedDelta, verified.dump().c_str());

        std::cout << "push of " << kFiles << " x " << kFileSize << " bytes (" << kPushBytes
                  << "): read " << readHashing << " bytes when libddb hashes, " << readVerified
                  << " with verified hashes" << std::endl;
        RecordProperty("pushBytes", std::to_string(kPushBytes));
        RecordProperty("bytesReadHashing", std::to_string(readHashing));
        RecordProperty("bytesReadVerified", std::to_string(readVerified));

        EXPECT_GE(readHashing, kPushBytes) << "every added byte is read once to hash it";
        EXPECT_LT(readVerified, kFileSize) << "a verified file was read again";
        for (const auto &add : trustedDelta.adds)
            EXPECT_EQ(hashOf(trusted.db.get(), add.path), add.hash);
    }
#endif

    TEST(DDB04_RemoveFromIndex, oneTransaction)
    {
        // A path without entries rolls back the removal of the paths before it
        Fixture f(TEST_NAME);
        std::vector<std::string> removed;
        EXPECT_THROW(removeFromIndex(f.db.get(), {(f.root / "keep.txt").string(), (f.root / "missing.txt").string()},
                                     [&removed](const std::string &p)
                                     { removed.push_back(p); }),
                     FSException);
        EXPECT_NE(hashOf(f.db.get(), "keep.txt"), "<missing>");
        EXPECT_TRUE(removed.empty()) << "a rolled-back removal was reported";

        removeFromIndex(f.db.get(), {(f.root / "keep.txt").string(), (f.root / "dir").string()},
                        [&removed](const std::string &p)
                        { removed.push_back(p); });
        std::sort(removed.begin(), removed.end());
        EXPECT_EQ(removed, (std::vector<std::string>{"dir", "dir/gone.txt", "keep.txt"}));
        EXPECT_EQ(hashOf(f.db.get(), "keep.txt"), "<missing>");
        EXPECT_EQ(hashOf(f.db.get(), "dir/gone.txt"), "<missing>");
    }

    TEST(DDB04_RemoveFromIndex, pathListedTwiceOrInsideAListedFolder)
    {
        // Removed once; the repeat must not fail (and roll back) the whole removal
        Fixture f(TEST_NAME);
        std::vector<std::string> removed;
        removeFromIndex(f.db.get(),
                        {(f.root / "keep.txt").string(), (f.root / "keep.txt").string(),
                         (f.root / "dir").string(), (f.root / "dir" / "gone.txt").string()},
                        [&removed](const std::string &p)
                        { removed.push_back(p); });
        std::sort(removed.begin(), removed.end());
        EXPECT_EQ(removed, (std::vector<std::string>{"dir", "dir/gone.txt", "keep.txt"}));
        EXPECT_EQ(hashOf(f.db.get(), "keep.txt"), "<missing>");
        EXPECT_EQ(hashOf(f.db.get(), "dir/gone.txt"), "<missing>");
        EXPECT_NE(hashOf(f.db.get(), "old.txt"), "<missing>");
    }

    TEST(DDB04_ApplyDelta, removeListedTwice)
    {
        Fixture f(TEST_NAME);
        const auto hash = hashOf(f.db.get(), "old.txt");
        Delta d;
        d.removes.push_back(RemoveAction("old.txt", hash));
        d.removes.push_back(RemoveAction("old.txt", hash));

        EXPECT_TRUE(f.apply(d).empty());
        EXPECT_FALSE(fs::exists(f.root / "old.txt"));
        EXPECT_EQ(hashOf(f.db.get(), "old.txt"), "<missing>");
    }

    TEST(DDB04_ApplyDelta, failedAddStillIndexesTheFilesAlreadyMoved)
    {
        Fixture f(TEST_NAME);
        writeFile(f.root / "blocker", "a file where the delta expects a folder");
        Delta d;
        d.adds.push_back(f.stage("a.txt", "alpha"));
        d.adds.push_back(f.stage("blocker/c.txt", "charlie"));

        EXPECT_ANY_THROW(f.apply(d));

        // a.txt left the source folder: the index must describe it, or nothing could
        // bring the dataset and the index back in step
        EXPECT_FALSE(fs::exists(f.source / "a.txt"));
        EXPECT_EQ(readFile(f.root / "a.txt"), "alpha");
        EXPECT_EQ(hashOf(f.db.get(), "a.txt"), Hash::fileSHA256((f.root / "a.txt").string()));
        EXPECT_EQ(hashOf(f.db.get(), "blocker/c.txt"), "<missing>");
    }

    TEST(DDB04_MoveOrCopy, renamesSingleLinkAndCopiesSharedData)
    {
        TestArea ta(TEST_NAME, true);
        const auto dir = ta.getFolder("files");
        writeFile(dir / "a", "a");
        writeFile(dir / "dest", "old");
        EXPECT_TRUE(io::moveOrCopy(dir / "a", dir / "dest"));
        EXPECT_FALSE(fs::exists(dir / "a"));
        EXPECT_EQ(readFile(dir / "dest"), "a");

        writeFile(dir / "b", "b");
        fs::create_hard_link(dir / "b", dir / "b-link");
        EXPECT_FALSE(io::moveOrCopy(dir / "b-link", dir / "b-copy"));
        EXPECT_FALSE(fs::exists(dir / "b-link"));
        EXPECT_EQ(readFile(dir / "b-copy"), "b");
        EXPECT_EQ(fs::hard_link_count(dir / "b"), 1u);
        EXPECT_EQ(fs::hard_link_count(dir / "b-copy"), 1u);

        EXPECT_THROW(io::moveOrCopy(dir / "missing", dir / "x"), FSException);
    }
} // namespace
