/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The command registry (every alias resolves, every command can be made), the option
// helpers, and the one handler that turns what a command throws into stderr and an exit code,
// with the commands' output (stdout) left clean.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cmd/include/cli.h"
#include "cmd/include/cmdlist.h"
#include "cmd/include/credentials.h"
#include "cmd/include/pull.h"
#include "cmd/include/push.h"
#include "cmd/include/rescan.h"
#include "entry_filters.h"
#include "entry_types.h"
#include "exceptions.h"
#include "fake_http_client.h"
#include "fake_model_tools.h"
#include "gtest/gtest.h"
#include "obj2tiles_runner.h"
#include "raster_test_utils.h"
#include "stringutil.h"
#include "tagmanager.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace {

using cmd::Command;
using cmd::CommandRegistry;

// A command the test scripts: one option, and a body that returns or throws what it is told
class FakeCommand : public Command {
public:
    explicit FakeCommand(std::function<int(cxxopts::ParseResult &)> work = [](cxxopts::ParseResult &) { return EXIT_SUCCESS; })
        : body(std::move(work)) {}

    std::string description() override { return "A fake command."; }

    int run(cxxopts::ParseResult &opts) override { return body(opts); }

    void setOptions(cxxopts::Options &opts) override {
        opts.positional_help("[args]").custom_help("fake [-n number]").add_options()(
            "n,number", "A number", cxxopts::value<int>()->default_value("1"));
        cmd::addWorkingDirOption(opts);
    }

    using Command::usageError;

private:
    std::function<int(cxxopts::ParseResult &)> body;
};

struct Outcome {
    int code = -1;
    std::string out;  // what the process wrote to stdout
    std::string err;
};

// Runs a command line through the handler, capturing stdout (the handler writes to `err`)
Outcome execute(Command &command, std::vector<std::string> args) {
    std::vector<char *> argv{const_cast<char *>("ddb")};
    for (auto &a : args)
        argv.push_back(a.data());

    std::ostringstream out, err;
    std::streambuf *previous = std::cout.rdbuf(out.rdbuf());
    Outcome outcome;
    try {
        outcome.code = cmd::execute(command, static_cast<int>(argv.size()), argv.data(), err);
    } catch (...) {
        std::cout.rdbuf(previous);
        throw;
    }
    std::cout.rdbuf(previous);
    outcome.out = out.str();
    outcome.err = err.str();
    return outcome;
}

// ------------------------------------------------------------------------ registry

TEST(CliRegistry, everyAliasResolvesToARegisteredCommand) {
    const auto &registry = CommandRegistry::builtin();
    const auto names = registry.names();
    const std::set<std::string> registered(names.begin(), names.end());

    ASSERT_FALSE(registry.aliases().empty());
    for (const auto &[alias, target] : registry.aliases()) {
        EXPECT_TRUE(registered.count(target)) << "alias '" << alias << "' names '" << target << "', which is not a command";
        EXPECT_FALSE(registered.count(alias)) << "alias '" << alias << "' hides a command";
        EXPECT_EQ(registry.resolve(alias), target);
        EXPECT_NE(registry.create(alias), nullptr) << alias;
    }
}

TEST(CliRegistry, attribAndAttrAreGoneWithTheCommandTheyPointedTo) {
    // They named chattr, a command that never existed
    const auto &registry = CommandRegistry::builtin();
    EXPECT_EQ(registry.create("chattr"), nullptr);
    EXPECT_EQ(registry.create("attrib"), nullptr);
    EXPECT_EQ(registry.create("attr"), nullptr);
}

TEST(CliRegistry, aDanglingOrRepeatedRegistrationIsRefused) {
    CommandRegistry registry;
    registry.add<FakeCommand>("fake");
    EXPECT_THROW(registry.add<FakeCommand>("fake"), std::logic_error);
    EXPECT_THROW(registry.alias("f", "missing"), std::logic_error);
    registry.alias("f", "fake");
    EXPECT_THROW(registry.alias("f", "fake"), std::logic_error);
    EXPECT_THROW(registry.alias("fake", "fake"), std::logic_error);
    EXPECT_EQ(registry.resolve("f"), "fake");
    EXPECT_EQ(registry.resolve("nope"), "");
}

TEST(CliRegistry, everyCommandIsMadeFreshAndDescribesItself) {
    const auto &registry = CommandRegistry::builtin();
    const auto names = registry.names();
    EXPECT_GE(names.size(), 37u);
    for (const auto &name : names) {
        const auto a = registry.create(name);
        const auto b = registry.create(name);
        ASSERT_NE(a, nullptr) << name;
        EXPECT_NE(a.get(), b.get()) << name << ": one object per request, no shared state";
        EXPECT_FALSE(a->description().empty()) << name;

        std::ostringstream help;
        a->printHelp(help);
        EXPECT_NE(help.str().find("Usage:"), std::string::npos) << name;
        EXPECT_NE(help.str().find("--help"), std::string::npos) << name;
        EXPECT_NE(help.str().find("--debug"), std::string::npos) << name;
        EXPECT_NE(help.str().find("ddb "), std::string::npos) << name << ": the program is called ddb";
    }
}

TEST(CliRegistry, namesAreSortedAsTheHelpListsThem) {
    const auto names = CommandRegistry::builtin().names();
    EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
}

TEST(CliRegistry, anUnknownNameMakesNothing) {
    EXPECT_EQ(CommandRegistry::builtin().create("nosuchcommand"), nullptr);
    EXPECT_EQ(CommandRegistry::builtin().resolve(""), "");
}

TEST(CliRegistry, everyCommandHelpRunsThroughTheHandler) {
    const auto &registry = CommandRegistry::builtin();
    for (const auto &name : registry.names()) {
        auto command = registry.create(name);
        const auto outcome = execute(*command, {"--help"});
        EXPECT_EQ(outcome.code, EXIT_SUCCESS) << name;
        EXPECT_NE(outcome.out.find("Usage:"), std::string::npos) << name;
        EXPECT_TRUE(outcome.err.empty()) << name << ": " << outcome.err;
    }
}

// ------------------------------------------------------------------------ option helpers

TEST(CliOptions, theSharedOptionsReadTheSameInEveryCommand) {
    const auto &registry = CommandRegistry::builtin();
    int workingDir = 0, insecure = 0, remote = 0, format = 0, output = 0, overwrite = 0;
    for (const auto &name : registry.names()) {
        std::ostringstream help;
        registry.create(name)->printHelp(help);
        const auto text = help.str();
        if (text.find("--working-dir") != std::string::npos) {
            workingDir++;
            EXPECT_NE(text.find("-w, --working-dir arg"), std::string::npos) << name;
            EXPECT_NE(text.find("Working directory (default: .)"), std::string::npos) << name;
        }
        if (text.find("--insecure") != std::string::npos) {
            insecure++;
            EXPECT_NE(text.find("-k, --insecure"), std::string::npos) << name;
            EXPECT_NE(text.find("Disable SSL certificate verification"), std::string::npos) << name;
        }
        if (text.find("--remote") != std::string::npos) {
            remote++;
            EXPECT_NE(text.find("The remote Registry (default: \"\")"), std::string::npos) << name;
        }
        if (text.find("--format") != std::string::npos) {
            format++;
            EXPECT_NE(text.find("-f, --format arg"), std::string::npos) << name;
            EXPECT_NE(text.find("(default: text)"), std::string::npos) << name;
        }
        if (text.find("--output") != std::string::npos)
            output++;
        if (text.find("--overwrite") != std::string::npos) {
            overwrite++;
            EXPECT_NE(text.find("Replace the output if it already exists"), std::string::npos) << name << ": " << text;
        }
    }
    // The commands that declare them
    EXPECT_EQ(workingDir, 14);
    EXPECT_EQ(insecure, 5);
    EXPECT_EQ(remote, 2);
    EXPECT_EQ(format, 7);
    EXPECT_EQ(output, 15);
    // cog, align, merge-multispectral, mask, geoproj, bm3d, gsplat, copc, tile, tiles3d, thumbs (D-028)
    EXPECT_EQ(overwrite, 11);
}

TEST(CliOptions, theFormatHelperNamesTheFormatsOfTheCommand) {
    cxxopts::Options one("ddb", "x"), two("ddb", "x");
    cmd::addFormatOption(one);
    cmd::addFormatOption(two, "text|json|geojson");
    EXPECT_NE(one.help().find("Output format (text|json)"), std::string::npos);
    EXPECT_NE(two.help().find("Output format (text|json|geojson)"), std::string::npos);
}

TEST(CliOptions, theFormatIsParsedOnceAtTheBoundaryOfTheCommand) {
    cxxopts::Options options("ddb", "x");
    cmd::addFormatOption(options, "text|json|geojson");
    auto parse = [&](std::vector<std::string> args) {
        std::vector<char *> argv{const_cast<char *>("ddb")};
        for (auto &a : args)
            argv.push_back(a.data());
        return options.parse(static_cast<int>(argv.size()), argv.data());
    };

    EXPECT_EQ(cmd::getFormatOption(parse({})), ddb::OutputFormat::Text);
    EXPECT_EQ(cmd::getFormatOption(parse({"-f", "json"})), ddb::OutputFormat::Json);
    EXPECT_EQ(cmd::getFormatOption(parse({"-f", "geojson"}), {ddb::OutputFormat::GeoJson}), ddb::OutputFormat::GeoJson);

    // what the command does not write is refused, with the same message whatever the reason
    EXPECT_THROW(cmd::getFormatOption(parse({"-f", "geojson"})), ddb::InvalidArgsException);
    EXPECT_THROW(cmd::getFormatOption(parse({"-f", "xml"})), ddb::InvalidArgsException);
    EXPECT_THROW(cmd::getFormatOption(parse({"-f", "JSON"})), ddb::InvalidArgsException);
}

// ------------------------------------------------------------------------ the handler

TEST(CliHandler, aCommandReturnsItsOwnExitCode) {
    FakeCommand ok;
    EXPECT_EQ(execute(ok, {}).code, EXIT_SUCCESS);

    FakeCommand seven([](cxxopts::ParseResult &) { return 7; });
    const auto outcome = execute(seven, {});
    EXPECT_EQ(outcome.code, 7);
    EXPECT_TRUE(outcome.err.empty());
}

TEST(CliHandler, theCommandReadsItsOptions) {
    int seen = 0;
    FakeCommand command([&seen](cxxopts::ParseResult &opts) {
        seen = opts["number"].as<int>();
        return EXIT_SUCCESS;
    });
    EXPECT_EQ(execute(command, {"-n", "42"}).code, EXIT_SUCCESS);
    EXPECT_EQ(seen, 42);
    EXPECT_EQ(execute(command, {}).code, EXIT_SUCCESS);
    EXPECT_EQ(seen, 1);
}

TEST(CliHandler, helpPrintsTheUsageOnStdoutAndSucceeds) {
    bool ran = false;
    FakeCommand command([&ran](cxxopts::ParseResult &) {
        ran = true;
        return EXIT_SUCCESS;
    });
    for (const char *flag : {"--help", "-h"}) {
        const auto outcome = execute(command, {flag});
        EXPECT_EQ(outcome.code, EXIT_SUCCESS) << flag;
        EXPECT_NE(outcome.out.find("A fake command."), std::string::npos) << flag;
        EXPECT_NE(outcome.out.find("-n, --number arg"), std::string::npos) << flag;
        EXPECT_TRUE(outcome.err.empty()) << flag;
    }
    EXPECT_FALSE(ran) << "help runs nothing";
}

TEST(CliHandler, aUsageErrorPrintsTheMessageAndTheUsageOnStderr) {
    FakeCommand command;
    FakeCommand failing([&command](cxxopts::ParseResult &) -> int { command.usageError("missing required argument 'x'"); });
    const auto outcome = execute(failing, {});
    EXPECT_EQ(outcome.code, EXIT_FAILURE);
    EXPECT_TRUE(outcome.out.empty()) << "stdout stays clean: " << outcome.out;
    EXPECT_EQ(outcome.err.find("error: missing required argument 'x'\n\n"), 0u) << outcome.err;
    EXPECT_NE(outcome.err.find("Usage:"), std::string::npos);
    EXPECT_NE(outcome.err.find("-n, --number arg"), std::string::npos);

    FakeCommand silent([&command](cxxopts::ParseResult &) -> int { command.usageError(); });
    const auto without = execute(silent, {});
    EXPECT_EQ(without.code, EXIT_FAILURE);
    EXPECT_EQ(without.err.find("error:"), std::string::npos) << "no message, only the usage";
    EXPECT_NE(without.err.find("Usage:"), std::string::npos);
}

TEST(CliHandler, optionsThatDoNotParseAreUsageErrors) {
    FakeCommand command;
    for (const std::vector<std::string> &args :
         {std::vector<std::string>{"--no-such-option"}, {"-n", "abc"}, {"-n"}}) {
        const auto outcome = execute(command, args);
        EXPECT_EQ(outcome.code, EXIT_FAILURE) << args[0];
        EXPECT_TRUE(outcome.out.empty());
        EXPECT_EQ(outcome.err.find("error: "), 0u) << outcome.err;
        EXPECT_NE(outcome.err.find("Usage:"), std::string::npos);
    }
}

TEST(CliHandler, invalidArgumentsOfTheLibraryAreUsageErrors) {
    FakeCommand command([](cxxopts::ParseResult &) -> int { throw ddb::InvalidArgsException("Invalid format xml"); });
    const auto outcome = execute(command, {});
    EXPECT_EQ(outcome.code, EXIT_FAILURE);
    EXPECT_TRUE(outcome.out.empty());
    EXPECT_EQ(outcome.err.find("error: Invalid format xml\n\n"), 0u) << outcome.err;
    EXPECT_NE(outcome.err.find("Usage:"), std::string::npos);
}

TEST(CliHandler, aFailedCommandPrintsItsMessageOnStderr) {
    FakeCommand command([](cxxopts::ParseResult &) -> int { throw ddb::AppException("Cannot push if no tag is specified"); });
    const auto outcome = execute(command, {});
    EXPECT_EQ(outcome.code, EXIT_FAILURE);
    EXPECT_TRUE(outcome.out.empty());
    EXPECT_EQ(outcome.err, "Cannot push if no tag is specified\n");

    FakeCommand fs([](cxxopts::ParseResult &) -> int { throw ddb::FSException("Cannot open x"); });
    EXPECT_EQ(execute(fs, {}).err, "Cannot open x\n");
    FakeCommand canceled([](cxxopts::ParseResult &) -> int { throw ddb::CanceledException("canceled"); });
    EXPECT_EQ(execute(canceled, {}).code, EXIT_FAILURE);
}

TEST(CliHandler, anyOtherExceptionIsAnErrorNotACrash) {
    FakeCommand command([](cxxopts::ParseResult &) -> int { throw std::runtime_error("disk on fire"); });
    const auto outcome = execute(command, {});
    EXPECT_EQ(outcome.code, EXIT_FAILURE);
    EXPECT_TRUE(outcome.out.empty());
    EXPECT_EQ(outcome.err, "error: disk on fire\n");

    FakeCommand alloc([](cxxopts::ParseResult &) -> int { throw std::bad_alloc(); });
    EXPECT_EQ(execute(alloc, {}).code, EXIT_FAILURE);
}

TEST(CliHandler, theOutputOfACommandReachesStdoutUntouchedWhenItFails) {
    FakeCommand command([](cxxopts::ParseResult &) -> int {
        std::cout << "{\"partial\":true}\n";
        throw ddb::AppException("then it failed");
    });
    const auto outcome = execute(command, {});
    EXPECT_EQ(outcome.code, EXIT_FAILURE);
    EXPECT_EQ(outcome.out, "{\"partial\":true}\n");
    EXPECT_EQ(outcome.err, "then it failed\n");
}

// ------------------------------------------------------------------------ real commands

TEST(CliCommands, aRealCommandWithoutItsArgumentsIsAUsageError) {
    for (const char* name :
         {"cog", "copc", "add", "remove", "geoproj", "mask", "bm3d", "thumbs", "tile"}) {
        auto command = CommandRegistry::builtin().create(name);
        const auto outcome = execute(*command, {});
        EXPECT_EQ(outcome.code, EXIT_FAILURE) << name;
        EXPECT_TRUE(outcome.out.empty()) << name << ": " << outcome.out;
        EXPECT_NE(outcome.err.find("Usage:"), std::string::npos) << name;
    }
}

// ddb build --threads N: the same rule as DDBBuildEx, refused as a usage error before the dataset
// is opened
TEST(CliCommands, buildRefusesAThreadCountThatIsNotAWholeNumberFromOneTo4096) {
    TestArea ta("cli_build_threads_refused", true);
    const std::string dataset = ta.getPath("not_a_dataset").string();
    for (const char *bad : {"0", "-1", "4097", "99999", "99999999999999", "two", "2.5", "", "1e2", "0x2"}) {
        auto command = CommandRegistry::builtin().create("build");
        const auto outcome = execute(*command, {"--threads", bad, "-w", dataset});
        EXPECT_EQ(outcome.code, EXIT_FAILURE) << "'" << bad << "'";
        EXPECT_TRUE(outcome.out.empty()) << outcome.out;
        EXPECT_NE(outcome.err.find("Usage:"), std::string::npos) << "'" << bad << "': " << outcome.err;
        // Refused by the option parser or by the rule of the cap, never by the dataset
        EXPECT_EQ(outcome.err.find("not_a_dataset"), std::string::npos) << "'" << bad << "': " << outcome.err;
    }
    auto command = CommandRegistry::builtin().create("build");
    const auto outcome = execute(*command, {"--threads", "0", "-w", dataset});
    EXPECT_NE(outcome.err.find("threads must be an integer from 1 to 4096"), std::string::npos) << outcome.err;
}

TEST(CliCommands, buildTakesAThreadCountFromOneTo4096) {
    const auto root = makeIndex("cli_build_threads_accepted");
    for (const char *threads : {"1", "2", "4096"}) {
        auto command = CommandRegistry::builtin().create("build");
        const auto outcome = execute(*command, {"--threads", threads, "-w", root.string()});
        EXPECT_EQ(outcome.code, EXIT_SUCCESS) << threads << ": " << outcome.err;
    }
    // --threads=N and the path of one entry
    auto command = CommandRegistry::builtin().create("build");
    EXPECT_EQ(execute(*command, {"--threads=2", "-p", "a.txt", "-w", root.string()}).code, EXIT_SUCCESS);
    // without it, as before
    auto plain = CommandRegistry::builtin().create("build");
    EXPECT_EQ(execute(*plain, {"-w", root.string()}).code, EXIT_SUCCESS);
}

#ifndef _WIN32
// --threads reaches the tools of the build: bm3d gets it as its own --threads, for the build
// of one entry (-p) and for the build of every entry
TEST(CliCommands, buildGivesTheThreadCountToTheToolsOfTheBuild) {
    testtools::ModelDataset ds("cli_build_threads_reach_bm3d");
    testtools::FakeModelTools tools(ds.index.ta);
    const ScopedProcessConfig pool(R"({"cpuPoolSize": 4096})");
    EnvVarGuard budget("DDB_BUILD_THREADS", "6");
    const std::string output = ds.index.db->buildDirectory().string();

    auto one = CommandRegistry::builtin().create("build");
    const auto entry = execute(*one, {"--threads", "2", "-f", "-p", "model.obj", "-o", output, "-w", ds.root()});
    ASSERT_EQ(entry.code, EXIT_SUCCESS) << entry.err;
    EXPECT_EQ(testtools::readText(tools.bm3dLog).rfind("--threads 2 ", 0), 0u)
        << testtools::readText(tools.bm3dLog);

    fs::remove(tools.bm3dLog);
    auto all = CommandRegistry::builtin().create("build");
    const auto every = execute(*all, {"--threads", "3", "-f", "-o", output, "-w", ds.root()});
    ASSERT_EQ(every.code, EXIT_SUCCESS) << every.err;
    EXPECT_EQ(testtools::readText(tools.bm3dLog).rfind("--threads 3 ", 0), 0u)
        << testtools::readText(tools.bm3dLog);
}
#endif

TEST(CliCommands, anUnknownMetaFormatIsAUsageErrorWithoutOutput) {
    TestArea ta("cli_info_format", true);
    const auto file = ta.getPath("a.txt");
    std::ofstream(file) << "alpha";
    auto command = CommandRegistry::builtin().create("info");
    const auto outcome = execute(*command, {"-f", "xml", file.string()});
    EXPECT_EQ(outcome.code, EXIT_FAILURE);
    EXPECT_TRUE(outcome.out.empty()) << outcome.out;
    EXPECT_NE(outcome.err.find("error: "), std::string::npos) << outcome.err;
    EXPECT_NE(outcome.err.find("Usage:"), std::string::npos);
}

// delta and tile wrote nothing, or text, for a format they did not know; list and search failed
// only once their index was open (and list had already truncated its output file)
TEST(CliCommands, everyCommandWithAFormatRefusesAnUnknownOneBeforeItDoesAnything) {
    TestArea ta("cli_unknown_format", true);
    const auto target = ta.getPath("out.txt");
    const std::vector<std::pair<std::string, std::vector<std::string>>> cases = {
        {"list", {"-f", "xml", "-o", target.string()}},
        {"list", {"-f", "geojson"}},
        {"search", {"-f", "geojson", "*"}},
        {"delta", {"-f", "xml", ta.getPath("a").string(), ta.getPath("b").string()}},
        {"tile", {"-f", "xml", ta.getPath("missing.tif").string()}},
        {"stamp", {"-f", "xml"}},
    };

    for (const auto &[name, args] : cases) {
        auto command = CommandRegistry::builtin().create(name);
        const auto outcome = execute(*command, args);
        EXPECT_EQ(outcome.code, EXIT_FAILURE) << name;
        EXPECT_TRUE(outcome.out.empty()) << name << ": " << outcome.out;
        EXPECT_NE(outcome.err.find("Invalid format"), std::string::npos) << name << ": " << outcome.err;
    }
    EXPECT_FALSE(std::filesystem::exists(target)) << "list created its output file before it knew the format";
}

// The help of --type listed the types by hand and missed those added since (gaussiansplat, tiles3d)
TEST(CliCommands, theTypeHelpOfRescanListsEveryTypeTheOptionTakes) {
    cmd::Rescan rescan;
    cxxopts::Options opts("ddb", rescan.description());
    rescan.setOptions(opts);
    std::string help;
    for (const auto &option : opts.group_help("").options)
        if (option.s == "t")
            help = option.desc;
    const std::string label = "Valid types: ";
    const size_t start = help.find(label);
    ASSERT_NE(start, std::string::npos) << help;

    std::vector<std::string> listed;
    for (const std::string &name : ddb::str::split(help.substr(start + label.size()), ','))
        listed.push_back(ddb::str::trimCopy(name));
    EXPECT_EQ(listed, ddb::getEntryTypeNames());
    // and each of them is taken by the option
    for (const std::string &name : listed)
        EXPECT_NO_THROW(ddb::filters::parseEntryTypeList(name.c_str())) << name;
}

TEST(CliCommands, infoWritesOnlyTheJsonToStdout) {
    TestArea ta("cli_info_json", true);
    const auto file = ta.getPath("a.txt");
    std::ofstream(file) << "alpha";
    auto command = CommandRegistry::builtin().create("info");
    const auto outcome = execute(*command, {"-f", "json", file.string()});
    EXPECT_EQ(outcome.code, EXIT_SUCCESS) << outcome.err;
    ASSERT_FALSE(outcome.out.empty());
    EXPECT_EQ(outcome.out.front(), '[');
    EXPECT_EQ(outcome.out.find_last_not_of(" \r\n"), outcome.out.rfind(']'));
}

// ddb gsplat x.spz with no output path used to convert x.spz to the default x.spz, replacing the
// file it reads; the same path, written another way, is the same file
TEST(CliCommands, gsplatNeverOverwritesItsInput) {
    TestArea ta("cli_gsplat_same_path", true);
    const auto spz = ta.getPath("scene.spz");
    const std::string content = std::string("\x1f\x8b", 2) + "an spz that is not to be touched";
    { std::ofstream out(spz, std::ios::binary); out << content; }
    auto readBack = [&] {
        std::ifstream in(spz, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };

    const std::vector<std::vector<std::string>> cases = {
        {spz.string()},                                      // the default output is the input
        {spz.string(), spz.string()},                        // the same path given
        {spz.string(), (ta.getPath("sub") / ".." / "scene.spz").string()},  // the same file by another path
        {"-o", spz.string(), spz.string()},
    };
    for (const auto &args : cases) {
        auto command = CommandRegistry::builtin().create("gsplat");
        const auto outcome = execute(*command, args);
        EXPECT_EQ(outcome.code, EXIT_FAILURE) << args.size();
        EXPECT_TRUE(outcome.out.empty()) << outcome.out;
        EXPECT_NE(outcome.err.find("is the input file"), std::string::npos) << outcome.err;
        EXPECT_NE(outcome.err.find("choose a different output path"), std::string::npos) << outcome.err;
        EXPECT_EQ(readBack(), content) << "the input was written to";
    }

    // Another extension converts to the default output next to it
    const auto splat = ta.getPath("other.splat");
    { std::ofstream out(splat, std::ios::binary); out << std::string(64, '\1'); }
    auto command = CommandRegistry::builtin().create("gsplat");
    const auto outcome = execute(*command, {splat.string()});
    EXPECT_EQ(outcome.code, EXIT_SUCCESS) << outcome.err;
    EXPECT_TRUE(std::filesystem::exists(ta.getPath("other.spz")));
}

// A DEM-less raster is enough: the command must get as far as opening the file. It never did:
// the defaults of --min and --max were the text "nan", which cxxopts cannot parse as a double
TEST(CliCommands, contourReachesItsWorkInsteadOfFailingToParseItsDefaults) {
    TestArea ta("cli_contour", true);
    auto command = CommandRegistry::builtin().create("contour");
    const auto outcome = execute(*command, {"-i", "1", ta.getPath("missing.tif").string()});
    EXPECT_EQ(outcome.code, EXIT_FAILURE);
    EXPECT_EQ(outcome.err.find("failed to parse"), std::string::npos) << outcome.err;
    EXPECT_EQ(outcome.err.find("Usage:"), std::string::npos) << "not a usage error: " << outcome.err;
}

// ------------------------------------------------------------------------ outputs (D-028)
//
// Every command that writes data files refuses an output that exists unless --overwrite, with
// one line that names the option, and an output that is one of its inputs even with it. The
// inputs of most refusals need not be what they claim: the check comes before any is read.

std::string readBytes(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::filesystem::path writeBytes(const std::filesystem::path &path, const std::string &content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
    return path;
}

Outcome runCommand(const std::string &name, std::vector<std::string> args) {
    auto command = CommandRegistry::builtin().create(name);
    if (!command)
        throw std::logic_error("no command " + name);
    return execute(*command, std::move(args));
}

void expectRefusedAsExisting(const Outcome &outcome, const std::string &label) {
    EXPECT_EQ(outcome.code, EXIT_FAILURE) << label << ": " << outcome.err;
    EXPECT_NE(outcome.err.find("add --overwrite to replace it"), std::string::npos) << label << ": " << outcome.err;
    EXPECT_EQ(outcome.err.find("Usage:"), std::string::npos) << label << ": one message, no usage";
    EXPECT_EQ(std::count(outcome.err.begin(), outcome.err.end(), '\n'), 1) << label << ": " << outcome.err;
}

void expectRefusedAsTheInput(const Outcome &outcome, const std::string &label) {
    EXPECT_EQ(outcome.code, EXIT_FAILURE) << label << ": " << outcome.err;
    EXPECT_NE(outcome.err.find("is the input file"), std::string::npos) << label << ": " << outcome.err;
}

// A small raster in Web Mercator: a square of data in a black border
std::filesystem::path smallRaster(const std::filesystem::path &path, double value, int bands = 3) {
    testutils::GeoRasterSpec spec;
    spec.width = spec.height = 64;
    spec.bands = bands;
    spec.value = [value](int band, int x, int y) {
        return x >= 8 && x < 56 && y >= 8 && y < 56 ? value + band * 7 + (x + y) % 5 : 0.0;
    };
    return testutils::createGeoRaster(path, spec);
}

// The cookbook said "ddb cog input.tif output.tif" (the order is output, input): with both
// present it replaced input.tif with the COG of output.tif
TEST(CliOutputSafety, cogNeverReplacesAnExistingOutputOrItsInputWithoutOverwrite) {
    TestArea ta("cli_cog_output", true);
    const auto a = smallRaster(ta.getPath("a.tif"), 100).string();
    const auto b = smallRaster(ta.getPath("b.tif"), 200).string();
    const auto aBytes = readBytes(a), bBytes = readBytes(b);

    expectRefusedAsExisting(runCommand("cog", {a, b}), "cog a.tif b.tif");
    EXPECT_EQ(readBytes(a), aBytes);
    EXPECT_EQ(readBytes(b), bBytes);

    for (const auto &args : std::vector<std::vector<std::string>>{
             {"-i", a, "-o", a}, {"--overwrite", "-i", a, "-o", a}, {"--overwrite", a, a}}) {
        expectRefusedAsTheInput(runCommand("cog", args), "the input as the output");
        EXPECT_EQ(readBytes(a), aBytes);
    }

    const auto replaced = runCommand("cog", {"--overwrite", a, b});
    EXPECT_EQ(replaced.code, EXIT_SUCCESS) << replaced.err;
    EXPECT_NE(readBytes(a), aBytes) << "--overwrite writes the COG of b.tif to a.tif";
    EXPECT_EQ(readBytes(b), bBytes);

    const auto out = ta.getPath("out.tif");
    const auto fresh = runCommand("cog", {"-i", b, "-o", out.string()});
    EXPECT_EQ(fresh.code, EXIT_SUCCESS) << fresh.err;
    EXPECT_TRUE(std::filesystem::exists(out));
}

TEST(CliOutputSafety, alignRefusesAnExistingOutputAndBothOfItsInputs) {
    TestArea ta("cli_align_output", true);
    const auto source = writeBytes(ta.getPath("source.tif"), "source").string();
    const auto reference = writeBytes(ta.getPath("reference.tif"), "reference").string();
    const auto output = writeBytes(ta.getPath("aligned.tif"), "keep").string();

    expectRefusedAsExisting(runCommand("align", {"-i", source, "-r", reference, "-o", output}), "align");
    for (const auto &input : {source, reference})
        expectRefusedAsTheInput(runCommand("align", {"--overwrite", "-i", source, "-r", reference, "-o", input}),
                                "align -o " + input);

    // With --overwrite the existing output is no reason to refuse (these files are no rasters)
    const auto passed = runCommand("align", {"--overwrite", "-i", source, "-r", reference, "-o", output});
    EXPECT_EQ(passed.code, EXIT_FAILURE);
    EXPECT_EQ(passed.err.find("already exists"), std::string::npos) << passed.err;

    EXPECT_EQ(readBytes(source), "source");
    EXPECT_EQ(readBytes(reference), "reference");
    EXPECT_EQ(readBytes(output), "keep");
}

TEST(CliOutputSafety, mergeMultispectralRefusesAnExistingOutputAndEachOfItsBands) {
    TestArea ta("cli_merge_output", true);
    std::vector<std::string> bands;
    for (int i = 0; i < 3; i++)
        bands.push_back(smallRaster(ta.getPath("band" + std::to_string(i) + ".tif"), 40.0 + 30 * i, 1).string());
    const auto lastBand = readBytes(bands.back());
    const auto output = writeBytes(ta.getPath("merged.tif"), "keep").string();
    auto args = [&](std::vector<std::string> before) {
        before.insert(before.end(), bands.begin(), bands.end());
        return before;
    };

    expectRefusedAsExisting(runCommand("merge-multispectral", args({"-o", output})), "merge");
    EXPECT_EQ(readBytes(output), "keep");

    expectRefusedAsTheInput(runCommand("merge-multispectral", args({"--overwrite", "-o", bands.back()})),
                            "merge onto a band");
    EXPECT_EQ(readBytes(bands.back()), lastBand);

    const auto written = runCommand("merge-multispectral", args({"--overwrite", "-o", output}));
    EXPECT_EQ(written.code, EXIT_SUCCESS) << written.err;
    EXPECT_NE(readBytes(output), "keep");
}

TEST(CliOutputSafety, maskRefusesAnExistingOutputAndItsInput) {
    TestArea ta("cli_mask_output", true);
    const auto input = smallRaster(ta.getPath("ortho.tif"), 128).string();
    const auto inputBytes = readBytes(input);
    const auto output = writeBytes(ta.getPath("ortho_masked.tif"), "keep").string();

    // The default output, <input>_masked.tif, and the one given
    expectRefusedAsExisting(runCommand("mask", {input}), "mask");
    expectRefusedAsExisting(runCommand("mask", {input, "-o", output}), "mask -o");
    EXPECT_EQ(readBytes(output), "keep");

    expectRefusedAsTheInput(runCommand("mask", {"--overwrite", input, "-o", input}), "mask onto its input");
    EXPECT_EQ(readBytes(input), inputBytes);

    const auto written = runCommand("mask", {"--overwrite", input});
    EXPECT_EQ(written.code, EXIT_SUCCESS) << written.err;
    EXPECT_NE(readBytes(output), "keep");
}

// The cookbook form (-o after the images) printed the usage: cxxopts gave the first positional to
// the output, so no image was left
TEST(CliOutputSafety, geoprojTakesItsOutputFirstOrFromOAndChecksEveryProjectedFile) {
    TestArea ta("cli_geoproj_output", true);
    const auto a = writeBytes(ta.getPath("images/a.JPG"), "not a photo").string();
    const auto b = writeBytes(ta.getPath("images/b.JPG"), "not a photo").string();
    const auto out = ta.getFolder("projected").string();
    const auto projectedA = writeBytes(ta.getPath("projected/a.tif"), "keep");

    for (const auto &args : std::vector<std::vector<std::string>>{
             {out + "/", a}, {a, "-o", out + "/"}, {"-o", out, a}, {out, "-i", a}}) {
        expectRefusedAsExisting(runCommand("geoproj", args), "geoproj " + args.front());
        EXPECT_EQ(readBytes(projectedA), "keep");
    }

    // Several images: nothing is written when one of the files exists
    expectRefusedAsExisting(runCommand("geoproj", {out, a, b}), "geoproj two images");
    EXPECT_FALSE(std::filesystem::exists(ta.getPath("projected/b.tif")));

    // One image to a file
    const auto file = writeBytes(ta.getPath("one.tif"), "keep");
    expectRefusedAsExisting(runCommand("geoproj", {file.string(), a}), "geoproj file");
    EXPECT_EQ(readBytes(file), "keep");

    // The output is the image
    expectRefusedAsTheInput(runCommand("geoproj", {"--overwrite", a, a}), "geoproj onto its image");
    EXPECT_EQ(readBytes(a), "not a photo");

    // Two images with one name
    writeBytes(ta.getPath("x/IMG.JPG"), "1");
    writeBytes(ta.getPath("y/IMG.JPG"), "2");
    const auto twice = runCommand("geoproj", {"--overwrite", ta.getPath("both").string() + "/",
                                              ta.getPath("x/IMG.JPG").string(), ta.getPath("y/IMG.JPG").string()});
    EXPECT_EQ(twice.code, EXIT_FAILURE);
    EXPECT_NE(twice.err.find("would both be written to"), std::string::npos) << twice.err;
    EXPECT_FALSE(std::filesystem::exists(ta.getPath("both")));

    // A missing folder named with a trailing separator is made (the photo is skipped: no GeoImage)
    const auto made = runCommand("geoproj", {ta.getPath("made").string() + "/", a});
    EXPECT_EQ(made.code, EXIT_SUCCESS) << made.err;
    EXPECT_TRUE(std::filesystem::is_directory(ta.getPath("made")));
}

TEST(CliOutputSafety, bm3dRefusesAnExistingOutputAndItsInput) {
    TestArea ta("cli_bm3d_output", true);
    const auto model = writeBytes(ta.getPath("m.obj"), "v 0 0 0\n").string();
    const auto output = writeBytes(ta.getPath("m.bm3d"), "keep").string();

    expectRefusedAsExisting(runCommand("bm3d", {model}), "bm3d (default output)");
    expectRefusedAsExisting(runCommand("bm3d", {model, output}), "bm3d");
    expectRefusedAsTheInput(runCommand("bm3d", {"--overwrite", model, model}), "bm3d onto its input");
    EXPECT_EQ(readBytes(output), "keep");
    EXPECT_EQ(readBytes(model), "v 0 0 0\n");
}

TEST(CliOutputSafety, gsplatReplacesAnExistingSpzOnlyWithOverwrite) {
    TestArea ta("cli_gsplat_output", true);
    const auto splat = writeBytes(ta.getPath("scene.splat"), std::string(64, '\1')).string();
    const auto output = writeBytes(ta.getPath("scene.spz"), "keep").string();

    expectRefusedAsExisting(runCommand("gsplat", {splat}), "gsplat (default output)");
    expectRefusedAsExisting(runCommand("gsplat", {splat, output}), "gsplat");
    EXPECT_EQ(readBytes(output), "keep");

    const auto written = runCommand("gsplat", {"--overwrite", splat, output});
    EXPECT_EQ(written.code, EXIT_SUCCESS) << written.err;
    EXPECT_NE(readBytes(output), "keep");
}

// ddb copc built in <output>/tmp and removed it (a tmp/ of the user's), and removed the COPC
// and legacy EPT files of the folder before it read its inputs (which could be one of them)
TEST(CliOutputSafety, copcTouchesNothingButItsOutput) {
    TestArea ta("cli_copc_output", true);
    EnvVarGuard backend("DDB_COPC_BACKEND", "pdal"), forcePdal("DDB_USE_PDAL_COPC");
    unsetEnv("DDB_USE_PDAL_COPC");
    const auto project = ta.getFolder("project");
    std::string points;
    for (int i = 0; i <= 10; i++)
        points += std::to_string(i) + " " + std::to_string((i * 7) % 11) + " " + std::to_string(i % 3) + "\n";
    const auto cloud = writeBytes(project / "cloud.xyz", points).string();
    const auto mine = writeBytes(project / "tmp" / "mine.txt", "mine");
    const auto legacy = writeBytes(project / "ept.json", "{}");

    const auto built = runCommand("copc", {project.string(), cloud});
    ASSERT_EQ(built.code, EXIT_SUCCESS) << built.err;
    const auto copc = project / "cloud.copc.laz";
    ASSERT_TRUE(std::filesystem::exists(copc));
    EXPECT_EQ(readBytes(mine), "mine") << "the tmp/ folder of the user is left alone";
    EXPECT_EQ(readBytes(cloud), points);
    EXPECT_TRUE(std::filesystem::exists(legacy)) << "only the build handler removes legacy EPT files";
    for (const auto &entry : std::filesystem::directory_iterator(project))
        EXPECT_NE(entry.path().filename().string().rfind(".copc-stage-", 0), 0u) << "the staging folder is gone";

    const auto copcBytes = readBytes(copc);
    expectRefusedAsExisting(runCommand("copc", {project.string(), cloud}), "copc again");
    expectRefusedAsTheInput(runCommand("copc", {"--overwrite", project.string(), copc.string()}), "copc of its output");
    EXPECT_EQ(readBytes(copc), copcBytes);

    const auto again = runCommand("copc", {"--overwrite", project.string(), cloud});
    EXPECT_EQ(again.code, EXIT_SUCCESS) << again.err;
    EXPECT_TRUE(std::filesystem::exists(copc));
    EXPECT_EQ(readBytes(mine), "mine");
}

TEST(CliOutputSafety, tileChecksEveryTileBeforeItWritesOne) {
    TestArea ta("cli_tile_output", true);
    const auto raster = smallRaster(ta.getPath("ortho.tif"), 128).string();
    const auto out = ta.getPath("tiles").string();
    const std::vector<std::string> args = {"-z", "14-15", raster, out};

    const auto first = runCommand("tile", args);
    ASSERT_EQ(first.code, EXIT_SUCCESS) << first.err;
    std::vector<std::filesystem::path> tiles;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(out))
        if (entry.is_regular_file())
            tiles.push_back(entry.path());
    ASSERT_EQ(tiles.size(), 2u);

    // One tile is there, the other is not: nothing is written
    writeBytes(tiles[0], "keep");
    std::filesystem::remove(tiles[1]);
    expectRefusedAsExisting(runCommand("tile", args), "tile again");
    EXPECT_EQ(readBytes(tiles[0]), "keep");
    EXPECT_FALSE(std::filesystem::exists(tiles[1]));

    std::vector<std::string> replace = args;
    replace.insert(replace.begin(), "--overwrite");
    const auto replaced = runCommand("tile", replace);
    EXPECT_EQ(replaced.code, EXIT_SUCCESS) << replaced.err;
    EXPECT_NE(readBytes(tiles[0]), "keep");
    EXPECT_TRUE(std::filesystem::exists(tiles[1]));
}

TEST(CliOutputSafety, tiles3dNeverReplacesTheFolderOfItsModel) {
    if (ddb::obj2tiles::findObj2TilesBinary().empty())
        GTEST_SKIP() << "Obj2Tiles is not installed: the command stops before it looks at its output";
    TestArea ta("cli_tiles3d_output", true);
    const auto models = ta.getFolder("models");
    const auto model = writeBytes(models / "m.obj", "v 0 0 0\n").string();
    const auto tileset = writeBytes(models / "3dtiles" / "tileset.json", "keep");

    const auto existing = runCommand("3dtiles", {model});
    expectRefusedAsExisting(existing, "tiles3d (default output)");
    EXPECT_NE(existing.err.find("is not empty"), std::string::npos) << existing.err;
    EXPECT_EQ(readBytes(tileset), "keep");

    const auto holder = runCommand("3dtiles", {"--overwrite", model, "-o", models.string()});
    EXPECT_EQ(holder.code, EXIT_FAILURE);
    EXPECT_NE(holder.err.find("holds the input"), std::string::npos) << holder.err;
    EXPECT_EQ(readBytes(model), "v 0 0 0\n");
}

TEST(CliOutputSafety, thumbsChecksEveryThumbnailBeforeItWritesOne) {
    TestArea ta("cli_thumbs_output", true);
    const auto a = writeBytes(ta.getPath("a.jpg"), "not a picture").string();
    const auto b = writeBytes(ta.getPath("b.jpg"), "not a picture").string();
    const auto out = ta.getFolder("thumbs");
    const auto thumbA = writeBytes(out / "a.webp", "keep");

    expectRefusedAsExisting(runCommand("thumbs", {a, b, "-o", out.string()}), "thumbs of two");
    EXPECT_EQ(readBytes(thumbA), "keep");
    EXPECT_FALSE(std::filesystem::exists(out / "b.webp"));

    // Two files with one name
    const auto x = writeBytes(ta.getPath("x/IMG.jpg"), "1").string();
    const auto y = writeBytes(ta.getPath("y/IMG.jpg"), "2").string();
    const auto twice = runCommand("thumbs", {"--overwrite", x, y, "-o", ta.getPath("both").string()});
    EXPECT_EQ(twice.code, EXIT_FAILURE);
    EXPECT_NE(twice.err.find("would both be written to"), std::string::npos) << twice.err;
    EXPECT_FALSE(std::filesystem::exists(ta.getPath("both")));

    // The thumbnail is the input
    const auto webp = writeBytes(ta.getPath("picture.webp"), "picture").string();
    expectRefusedAsTheInput(runCommand("thumbs", {"--overwrite", webp, "-o", webp}), "thumbs onto its input");
    EXPECT_EQ(readBytes(webp), "picture");

    // One raster to one file
    const auto raster = smallRaster(ta.getPath("ortho.tif"), 128).string();
    const auto thumb = writeBytes(ta.getPath("ortho.webp"), "keep");
    expectRefusedAsExisting(runCommand("thumbs", {raster, "-o", thumb.string()}), "thumbs of one");
    EXPECT_EQ(readBytes(thumb), "keep");
    const auto written = runCommand("thumbs", {"--overwrite", raster, "-o", thumb.string()});
    EXPECT_EQ(written.code, EXIT_SUCCESS) << written.err;
    EXPECT_EQ(readBytes(thumb).rfind("RIFF", 0), 0u) << "a WebP replaced it";
}

// ------------------------------------------------------------------------ offline

// Answers every question with alice/pw and counts them
class CountingCredentials : public ddb::CredentialProvider {
public:
    int asked = 0;
    AuthCredentials credentialsFor(const std::string &) override {
        asked++;
        return AuthCredentials("alice", "pw");
    }
};

// A dataset tagged for a Registry nobody can reach, as the current folder of the command
struct OfflineDataset {
    static constexpr const char *Url = "https://registry.test";

    IndexFixture dataset;
    fake::Network net;
    CountingCredentials credentials;
    std::filesystem::path previous = std::filesystem::current_path();

    explicit OfflineDataset(const std::string &name)
        : dataset(name), net(dataset.ta.getPath("auth.json")) {
        ddb::TagManager(dataset.db.get()).setTag(std::string(Url) + "/org/ds");
        dataset.db->close();
        net.http.on("POST", "/users/authenticate", fake::transportFailure());
        std::filesystem::current_path(dataset.root);
    }
    ~OfflineDataset() { std::filesystem::current_path(previous); }

    cmd::RegistryServices services() { return {&credentials, net.deps()}; }
};

// A refused connection or an unresolvable host during the login was an AuthException, so the
// operation asked for a password it could not use: ddb push offline prompted, then failed
TEST(CliCommands, pushAndPullOfflineFailWithTheTransportErrorAndDoNotPrompt) {
    for (const bool saved : {true, false}) {
        OfflineDataset f(saved ? "cli_offline_saved" : "cli_offline_unsaved");
        if (saved)
            f.net.credentials.saveCredentials(OfflineDataset::Url, AuthCredentials("alice", "pw"));

        cmd::Push push(f.services());
        cmd::Pull pull(f.services());
        for (cmd::Command *command : {static_cast<cmd::Command *>(&push), static_cast<cmd::Command *>(&pull)}) {
            f.credentials.asked = 0;
            const auto outcome = execute(*command, {});

            EXPECT_EQ(outcome.code, EXIT_FAILURE);
            EXPECT_TRUE(outcome.out.empty()) << outcome.out;
            EXPECT_NE(outcome.err.find("could not connect to https://registry.test: Connection refused"), std::string::npos)
                << outcome.err;
            // Nothing saved: the one login needs the credentials once. Saved: nobody is asked.
            EXPECT_EQ(f.credentials.asked, saved ? 0 : 1);
        }
    }
}

}  // namespace
