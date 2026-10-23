/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-12: the command registry (every alias resolves, every command can be made), the option
// helpers, and the one handler that turns what a command throws into stderr and an exit code,
// with the commands' output (stdout) left clean.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cmd/include/cli.h"
#include "cmd/include/cmdlist.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

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

TEST(Dq12Registry, everyAliasResolvesToARegisteredCommand) {
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

TEST(Dq12Registry, attribAndAttrAreGoneWithTheCommandTheyPointedTo) {
    // They named chattr, a command that never existed
    const auto &registry = CommandRegistry::builtin();
    EXPECT_EQ(registry.create("chattr"), nullptr);
    EXPECT_EQ(registry.create("attrib"), nullptr);
    EXPECT_EQ(registry.create("attr"), nullptr);
}

TEST(Dq12Registry, aDanglingOrRepeatedRegistrationIsRefused) {
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

TEST(Dq12Registry, everyCommandIsMadeFreshAndDescribesItself) {
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

TEST(Dq12Registry, namesAreSortedAsTheHelpListsThem) {
    const auto names = CommandRegistry::builtin().names();
    EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
}

TEST(Dq12Registry, anUnknownNameMakesNothing) {
    EXPECT_EQ(CommandRegistry::builtin().create("nosuchcommand"), nullptr);
    EXPECT_EQ(CommandRegistry::builtin().resolve(""), "");
}

TEST(Dq12Registry, everyCommandHelpRunsThroughTheHandler) {
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

TEST(Dq12Options, theSharedOptionsReadTheSameInEveryCommand) {
    const auto &registry = CommandRegistry::builtin();
    int workingDir = 0, insecure = 0, remote = 0, format = 0, output = 0;
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
    }
    // The commands that declare them (README of DQ-12: 14, 5, 2, 7 and 15 copies before)
    EXPECT_EQ(workingDir, 14);
    EXPECT_EQ(insecure, 5);
    EXPECT_EQ(remote, 2);
    EXPECT_EQ(format, 7);
    EXPECT_EQ(output, 15);
}

TEST(Dq12Options, theFormatHelperNamesTheFormatsOfTheCommand) {
    cxxopts::Options one("ddb", "x"), two("ddb", "x");
    cmd::addFormatOption(one);
    cmd::addFormatOption(two, "text|json|geojson");
    EXPECT_NE(one.help().find("Output format (text|json)"), std::string::npos);
    EXPECT_NE(two.help().find("Output format (text|json|geojson)"), std::string::npos);
}

TEST(Dq12Options, theFormatIsParsedOnceAtTheBoundaryOfTheCommand) {
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

TEST(Dq12Handler, aCommandReturnsItsOwnExitCode) {
    FakeCommand ok;
    EXPECT_EQ(execute(ok, {}).code, EXIT_SUCCESS);

    FakeCommand seven([](cxxopts::ParseResult &) { return 7; });
    const auto outcome = execute(seven, {});
    EXPECT_EQ(outcome.code, 7);
    EXPECT_TRUE(outcome.err.empty());
}

TEST(Dq12Handler, theCommandReadsItsOptions) {
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

TEST(Dq12Handler, helpPrintsTheUsageOnStdoutAndSucceeds) {
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

TEST(Dq12Handler, aUsageErrorPrintsTheMessageAndTheUsageOnStderr) {
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

TEST(Dq12Handler, optionsThatDoNotParseAreUsageErrors) {
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

TEST(Dq12Handler, invalidArgumentsOfTheLibraryAreUsageErrors) {
    FakeCommand command([](cxxopts::ParseResult &) -> int { throw ddb::InvalidArgsException("Invalid format xml"); });
    const auto outcome = execute(command, {});
    EXPECT_EQ(outcome.code, EXIT_FAILURE);
    EXPECT_TRUE(outcome.out.empty());
    EXPECT_EQ(outcome.err.find("error: Invalid format xml\n\n"), 0u) << outcome.err;
    EXPECT_NE(outcome.err.find("Usage:"), std::string::npos);
}

TEST(Dq12Handler, aFailedCommandPrintsItsMessageOnStderr) {
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

TEST(Dq12Handler, anyOtherExceptionIsAnErrorNotACrash) {
    FakeCommand command([](cxxopts::ParseResult &) -> int { throw std::runtime_error("disk on fire"); });
    const auto outcome = execute(command, {});
    EXPECT_EQ(outcome.code, EXIT_FAILURE);
    EXPECT_TRUE(outcome.out.empty());
    EXPECT_EQ(outcome.err, "error: disk on fire\n");

    FakeCommand alloc([](cxxopts::ParseResult &) -> int { throw std::bad_alloc(); });
    EXPECT_EQ(execute(alloc, {}).code, EXIT_FAILURE);
}

TEST(Dq12Handler, theOutputOfACommandReachesStdoutUntouchedWhenItFails) {
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

TEST(Dq12Commands, aRealCommandWithoutItsArgumentsIsAUsageError) {
    for (const char *name : {"cog", "copc", "add", "remove", "geoproj", "mask", "nxs", "thumbs", "tile"}) {
        auto command = CommandRegistry::builtin().create(name);
        const auto outcome = execute(*command, {});
        EXPECT_EQ(outcome.code, EXIT_FAILURE) << name;
        EXPECT_TRUE(outcome.out.empty()) << name << ": " << outcome.out;
        EXPECT_NE(outcome.err.find("Usage:"), std::string::npos) << name;
    }
}

TEST(Dq12Commands, anUnknownMetaFormatIsAUsageErrorWithoutOutput) {
    TestArea ta("dq12_info_format", true);
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
TEST(Dq12Commands, everyCommandWithAFormatRefusesAnUnknownOneBeforeItDoesAnything) {
    TestArea ta("dq12_unknown_format", true);
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

TEST(Dq12Commands, infoWritesOnlyTheJsonToStdout) {
    TestArea ta("dq12_info_json", true);
    const auto file = ta.getPath("a.txt");
    std::ofstream(file) << "alpha";
    auto command = CommandRegistry::builtin().create("info");
    const auto outcome = execute(*command, {"-f", "json", file.string()});
    EXPECT_EQ(outcome.code, EXIT_SUCCESS) << outcome.err;
    ASSERT_FALSE(outcome.out.empty());
    EXPECT_EQ(outcome.out.front(), '[');
    EXPECT_EQ(outcome.out.find_last_not_of(" \r\n"), outcome.out.rfind(']'));
}

// A DEM-less raster is enough: the command must get as far as opening the file. It never did:
// the defaults of --min and --max were the text "nan", which cxxopts cannot parse as a double
TEST(Dq12Commands, contourReachesItsWorkInsteadOfFailingToParseItsDefaults) {
    TestArea ta("dq12_contour", true);
    auto command = CommandRegistry::builtin().create("contour");
    const auto outcome = execute(*command, {"-i", "1", ta.getPath("missing.tif").string()});
    EXPECT_EQ(outcome.code, EXIT_FAILURE);
    EXPECT_EQ(outcome.err.find("failed to parse"), std::string::npos) << outcome.err;
    EXPECT_EQ(outcome.err.find("Usage:"), std::string::npos) << "not a usage error: " << outcome.err;
}

}  // namespace
