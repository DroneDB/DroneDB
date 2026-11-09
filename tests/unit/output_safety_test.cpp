/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// io::assureReplaceable and its forms (D-028): the one check that every writer of a data file
// makes before it writes, and io::isAtOrInside

#include <fstream>
#include <string>

#include "exceptions.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    void writeText(const fs::path &p, const std::string &content)
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << content;
    }

    // ─── One output ─────────────────────────────────────────────────────────────

    TEST(OutputSafety, anOutputThatIsAnInputIsRefusedEvenWithOverwrite)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path input = ta.getPath("in.tif");
        writeText(input, "in");
        for (const bool overwrite : {false, true})
        {
            // The input check comes first: an existing output that is the input is "the input"
            try
            {
                io::assureReplaceable(ta.getPath("sub") / ".." / "in.tif", overwrite, {ta.getPath("other.tif"), input});
                ADD_FAILURE() << "the input was accepted as the output";
            }
            catch (const OutputExistsException &e)
            {
                ADD_FAILURE() << "refused as an existing output, not as the input: " << e.what();
            }
            catch (const InvalidArgsException &e)
            {
                EXPECT_NE(std::string(e.what()).find("is the input file"), std::string::npos) << e.what();
            }
        }
    }

    TEST(OutputSafety, anExistingOutputIsRefusedUnlessOverwrite)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path output = ta.getPath("out.tif");
        EXPECT_NO_THROW(io::assureReplaceable(output, false, {ta.getPath("in.tif")})) << "nothing there yet";

        writeText(output, "keep");
        try
        {
            io::assureReplaceable(output, false, {ta.getPath("in.tif")});
            ADD_FAILURE() << "an existing output was accepted";
        }
        catch (const OutputExistsException &e)
        {
            EXPECT_EQ(e.path(), io::toUtf8(output));
            EXPECT_FALSE(e.isFolder());
            EXPECT_EQ(std::string(e.what()), "The output " + io::toUtf8(output) + " already exists: set overwrite to replace it");
        }
        EXPECT_NO_THROW(io::assureReplaceable(output, true, {ta.getPath("in.tif")}));

        // A folder at the path of a file is something that exists too
        EXPECT_THROW(io::assureReplaceable(ta.getFolder("folder"), false), OutputExistsException);
    }

    // ─── Several outputs ────────────────────────────────────────────────────────

    TEST(OutputSafety, everyOutputOfARunIsCheckedBeforeAnyIsWritten)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path a = ta.getPath("a.jpg"), b = ta.getPath("b.jpg");
        writeText(a, "a");
        writeText(b, "b");
        const std::vector<std::pair<fs::path, fs::path>> targets = {{a, ta.getPath("out/a.tif")},
                                                                    {b, ta.getPath("out/b.tif")}};
        EXPECT_NO_THROW(io::assureReplaceable(targets, false));

        // The second exists: refused, by its path
        writeText(ta.getPath("out/b.tif"), "keep");
        try
        {
            io::assureReplaceable(targets, false);
            ADD_FAILURE() << "an existing output was accepted";
        }
        catch (const OutputExistsException &e)
        {
            EXPECT_EQ(e.path(), io::toUtf8(ta.getPath("out/b.tif")));
        }
        EXPECT_NO_THROW(io::assureReplaceable(targets, true));

        // An output that is another pair's input
        EXPECT_THROW(io::assureReplaceable({{a, ta.getPath("out/a.tif")}, {b, a}}, true), InvalidArgsException);
    }

    TEST(OutputSafety, twoInputsWithTheSameTargetAreRefusedEvenWithOverwrite)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path first = ta.getPath("a/IMG.JPG"), second = ta.getPath("b/IMG.JPG");
        writeText(first, "1");
        writeText(second, "2");
        for (const bool overwrite : {false, true})
        {
            try
            {
                io::assureReplaceable({{first, ta.getPath("out/IMG.tif")}, {second, ta.getPath("out") / "." / "IMG.tif"}},
                                      overwrite);
                ADD_FAILURE() << "two inputs were given one output";
            }
            catch (const InvalidArgsException &e)
            {
                EXPECT_NE(std::string(e.what()).find("would both be written to"), std::string::npos) << e.what();
            }
        }
#if defined(_WIN32) || defined(__APPLE__)
        // Names that differ in case only are one file there
        EXPECT_THROW(io::assureReplaceable({{first, ta.getPath("out/IMG.tif")}, {second, ta.getPath("out/img.TIF")}}, true),
                     InvalidArgsException);
#endif
    }

    TEST(OutputSafety, aHardLinkToAnInputIsThatInput)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path input = ta.getPath("in.jpg");
        writeText(input, "in");
        const fs::path link = ta.getPath("out/in.tif");
        fs::create_directories(link.parent_path());
        std::error_code ec;
        fs::create_hard_link(input, link, ec);
        if (ec)
            GTEST_SKIP() << "no hard links here: " << ec.message();
        EXPECT_THROW(io::assureReplaceable({{input, link}}, true), InvalidArgsException);
        EXPECT_THROW(io::assureReplaceable(link, true, {input}), InvalidArgsException);
    }

    // ─── A folder replaced whole ────────────────────────────────────────────────

    TEST(OutputSafety, aFolderThatHoldsAnInputIsNeverReplaced)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path models = ta.getFolder("models");
        const fs::path model = models / "m.obj";
        writeText(model, "v 0 0 0");
        for (const bool overwrite : {false, true})
        {
            EXPECT_THROW(io::assureReplaceableFolder(models, overwrite, {model}), InvalidArgsException);
            EXPECT_THROW(io::assureReplaceableFolder(models, overwrite, {models}), InvalidArgsException);
        }
        try
        {
            io::assureReplaceableFolder(models / "sub" / "..", true, {model});
            ADD_FAILURE() << "the folder of the input was accepted";
        }
        catch (const InvalidArgsException &e)
        {
            EXPECT_NE(std::string(e.what()).find("holds the input"), std::string::npos) << e.what();
        }
    }

    TEST(OutputSafety, aFolderWithFilesIsReplacedOnlyWithOverwrite)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path model = ta.getPath("m.obj");
        writeText(model, "v 0 0 0");
        const fs::path tiles = ta.getPath("3dtiles");
        EXPECT_NO_THROW(io::assureReplaceableFolder(tiles, false, {model})) << "missing";
        fs::create_directories(tiles);
        EXPECT_NO_THROW(io::assureReplaceableFolder(tiles, false, {model})) << "empty";

        writeText(tiles / "tileset.json", "{}");
        try
        {
            io::assureReplaceableFolder(tiles, false, {model});
            ADD_FAILURE() << "a folder with files was accepted";
        }
        catch (const OutputExistsException &e)
        {
            EXPECT_TRUE(e.isFolder());
            EXPECT_EQ(e.path(), io::toUtf8(tiles));
            EXPECT_NE(std::string(e.what()).find("is not empty: set overwrite to replace it"), std::string::npos)
                << e.what();
        }
        EXPECT_NO_THROW(io::assureReplaceableFolder(tiles, true, {model}));
    }

    TEST(OutputSafety, isAtOrInsideTellsAFolderAndWhatIsBelowIt)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path folder = ta.getFolder("folder");
        EXPECT_TRUE(io::isAtOrInside(folder, folder));
        EXPECT_TRUE(io::isAtOrInside(folder / "a" / "b.txt", folder));
        EXPECT_TRUE(io::isAtOrInside(folder / ".." / "folder" / "x", folder));
        EXPECT_FALSE(io::isAtOrInside(ta.getPath("folderX/x"), folder)) << "a prefix of the name is not a parent";
        EXPECT_FALSE(io::isAtOrInside(ta.getPath(""), folder)) << "the parent";
    }

} // namespace
