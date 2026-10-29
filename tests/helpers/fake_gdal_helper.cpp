/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// A stand-in for gdal-util (apps/gdal-util/PROTOCOL.md) for the tests of the raster helper client.
// It answers `--version` and `--serve` like the real program, and answers or breaks the way the
// mode says. The mode of a request is the start of the name of its file (so one helper serves
// good and bad requests in any order), else the value of DDB_FAKE_GDAL_MODE, else "ok":
//
//   ok            a canned GeoRaster for a raster probe, a canned Vector for a vector probe, a
//                 valid (tiny) WebP written to `out` for a thumbnail, {} for any other operation
//   plain         a probe answers a raster without a projection
//   crash         exits in the middle of the request, without answering
//   hang          never answers (and stays alive until it is killed)
//   garbage       answers a line that is not JSON
//   huge          answers a line of 20 MiB
//   wrongid       answers another request's id
//   badresult     answers ok with a result the protocol does not allow
//   unreadable, policy, limit, crashed, timeout, nospace, internal
//                 answers the failure of that name
//   garbagethumb  a thumbnail is written as bytes that are not an image
//   hugethumb     a thumbnail is written as a file larger than any thumbnail can be
//   linkthumb     a thumbnail is a symbolic link to the request's file
//   sleep         waits 300 ms, then answers as "ok"
//
// Environment, read when the program starts:
//   DDB_FAKE_GDAL_MODE          the mode of the requests whose file name names none
//   DDB_FAKE_GDAL_PROTOCOL      the protocol `--version` reports (default "1.0")
//   DDB_FAKE_GDAL_LOG           a file where every request is appended as "<pid> <op> <file name>"
//   DDB_FAKE_GDAL_VERSION_LOG   a file where every run of `--version` appends "<pid> --version"
//   DDB_FAKE_GDAL_ENV_LOG       a file where the helper appends "<pid> <NAME>=<value>" for the
//                               variables it was started with that the host sets
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#define FAKE_GETPID _getpid
#else
#include <fcntl.h>
#include <unistd.h>
#define FAKE_GETPID getpid
#endif

namespace {

std::string envOr(const char *name, const std::string &fallback) {
    const char *value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : fallback;
}

// The value of a string member of the request line, undecoded ("" when it has none). Enough for
// the paths the tests use: no escape sequence other than \" and \\.
std::string stringMember(const std::string &line, const std::string &name) {
    const std::string marker = "\"" + name + "\":\"";
    const size_t start = line.find(marker);
    if (start == std::string::npos) return {};
    std::string value;
    for (size_t i = start + marker.size(); i < line.size() && line[i] != '"'; i++) {
        if (line[i] == '\\' && i + 1 < line.size()) i++;
        value += line[i];
    }
    return value;
}

std::string idOf(const std::string &line) {
    const size_t start = line.find("\"id\":");
    if (start == std::string::npos) return "null";
    size_t end = start + 5;
    while (end < line.size() && (line[end] == '-' || (line[end] >= '0' && line[end] <= '9'))) end++;
    return line.substr(start + 5, end - start - 5);
}

std::string baseName(const std::string &path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool startsWith(const std::string &text, const std::string &prefix) {
    return text.rfind(prefix, 0) == 0;
}

// Appends "<pid> <text>" to the log at `path`, in one write on a file opened for appending (the
// helpers of a pool write their logs at the same moment, and the test counts the lines)
void appendToLog(const std::string &path, const std::string &text) {
    const std::string line = std::to_string(FAKE_GETPID()) + " " + text + "\n";
    bool written = false;
#ifdef _WIN32
    const HANDLE file = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD count = 0;
        written = WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &count, nullptr) != 0 &&
                  count == line.size();
        CloseHandle(file);
    }
#else
    const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (file >= 0) {
        written = ::write(file, line.data(), line.size()) == static_cast<ssize_t>(line.size());
        ::close(file);
    }
#endif
    if (!written) {
        std::fprintf(stderr, "ddbtest_fake_gdal: cannot append to %s\n", path.c_str());
        std::fflush(stderr);
        std::_Exit(70);
    }
}

void answer(const std::string &text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

std::string failure(const std::string &id, const std::string &code, const std::string &message) {
    return "{\"id\":" + id + ",\"ok\":false,\"error\":\"" + code + "\",\"message\":\"" + message + "\"}";
}

const char *kModes[] = {"ok",        "plain",        "crash",    "crashed",   "hang",     "garbagethumb", "garbage",
                        "hugethumb", "huge",         "linkthumb", "wrongid",  "badresult", "unreadable",   "policy",
                        "limit",     "timeout",      "nospace",  "internal", "sleep"};

// The longest mode the name starts with ("crashed" is not "crash"), else the default
std::string modeOf(const std::string &name) {
    std::string found;
    for (const char *mode : kModes)
        if (startsWith(name, mode) && std::string(mode).size() > found.size()) found = mode;
    return found.empty() ? envOr("DDB_FAKE_GDAL_MODE", "ok") : found;
}

const char *kWkt = "GEOGCS[\\\"WGS 84\\\",DATUM[\\\"WGS_1984\\\",SPHEROID[\\\"WGS 84\\\",6378137,298.257223563]],"
                   "PRIMEM[\\\"Greenwich\\\",0],UNIT[\\\"degree\\\",0.0174532925199433]]";

std::string cannedProbe(const std::string &id, const std::string &line, const std::string &mode) {
    std::string result;
    if (line.find("\"kind\":\"vector\"") != std::string::npos) {
        result = "{\"type\":\"Vector\",\"isCog\":false,\"properties\":{\"vector\":{\"driver\":\"GeoJSON\",\"layers\":["
                 "{\"name\":\"fake\",\"geometryType\":\"Point\",\"featureCount\":3,\"extent\":[1,2,3,4]}]}},"
                 "\"pointGeom\":{\"type\":\"Point\",\"coordinates\":[2,3,0]},"
                 "\"polygonGeom\":{\"type\":\"Polygon\",\"coordinates\":[[[1,2,0],[3,2,0],[3,4,0],[1,4,0],[1,2,0]]]}}";
    } else if (mode == "plain") {
        result = "{\"type\":\"Raster\",\"isCog\":false,\"properties\":{}}";
    } else {
        result = std::string("{\"type\":\"GeoRaster\",\"isCog\":false,\"properties\":{\"width\":10,\"height\":20,") +
                 "\"geotransform\":[1,0.5,0,2,0,-0.5],\"bands\":[{\"type\":\"Byte\",\"colorInterp\":\"Gray\"}]},"
                 "\"wkt\":\"" + kWkt + "\","
                 "\"pointGeom\":{\"type\":\"Point\",\"coordinates\":[3.5,1,0]},"
                 "\"polygonGeom\":{\"type\":\"Polygon\",\"coordinates\":[[[1,2,0],[6,2,0],[6,-8,0],[1,-8,0],[1,2,0]]]}}";
    }
    return "{\"id\":" + id + ",\"ok\":true,\"result\":" + result + "}";
}

// The smallest file with the container of a WebP: "RIFF", the size of the rest, "WEBP", one chunk
std::string tinyWebp() {
    std::string bytes = "RIFF";
    const unsigned size = 4 + 8 + 8;
    for (int i = 0; i < 4; i++) bytes += static_cast<char>((size >> (8 * i)) & 0xff);
    bytes += "WEBPVP8L";
    bytes += std::string("\x08\x00\x00\x00", 4);
    bytes += std::string(8, '\0');
    return bytes;
}

void writeFile(const std::string &path, const std::string &content) {
    std::ofstream out(path, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

void answerThumbnail(const std::string &id, const std::string &line, const std::string &mode) {
    const std::string out = stringMember(line, "out");
    if (mode == "garbagethumb") {
        writeFile(out, "this is not an image, whatever the name says");
    } else if (mode == "hugethumb") {
        // Past the largest thumbnail the host accepts (64 MiB), as a sparse file
        std::ofstream file(out, std::ios::binary);
        file.seekp(65 * 1024 * 1024);
        file.put('\0');
    } else if (mode == "linkthumb") {
#ifndef _WIN32
        if (::symlink(stringMember(line, "path").c_str(), out.c_str()) != 0) std::_Exit(71);
#endif
    } else {
        writeFile(out, tinyWebp());
    }
    answer("{\"id\":" + id + ",\"ok\":true,\"result\":{\"bytes\":28}}");
}

int serve() {
    const std::string log = envOr("DDB_FAKE_GDAL_LOG", "");
    const std::string envLog = envOr("DDB_FAKE_GDAL_ENV_LOG", "");
    if (!envLog.empty())
        for (const char *name : {"MALLOC_ARENA_MAX", "PROJ_NETWORK"})
            appendToLog(envLog, std::string(name) + "=" + envOr(name, "unset"));

    std::string line;
    while (std::getline(std::cin, line)) {
        const std::string id = idOf(line);
        const std::string op = stringMember(line, "op");
        const std::string name = baseName(stringMember(line, "path"));
        const std::string mode = modeOf(name);

        if (!log.empty()) appendToLog(log, op + " " + name);

        if (mode == "crash") {
            std::_Exit(7);
        } else if (mode == "hang") {
            for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
        } else if (mode == "garbage") {
            answer("this is not json");
        } else if (mode == "huge") {
            const std::string chunk(1024 * 1024, 'x');
            std::fputs("{\"id\":", stdout);
            for (int i = 0; i < 20; i++) std::fwrite(chunk.data(), 1, chunk.size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        } else if (mode == "wrongid") {
            answer("{\"id\":987654321,\"ok\":true,\"result\":{}}");
        } else if (mode == "badresult") {
            answer("{\"id\":" + id + ",\"ok\":true,\"result\":{\"type\":\"Nonsense\",\"properties\":{}}}");
        } else if (mode == "unreadable" || mode == "policy" || mode == "limit" || mode == "crashed" ||
                   mode == "timeout" || mode == "nospace" || mode == "internal") {
            answer(failure(id, mode, "the fake helper fails with " + mode));
        } else {
            if (mode == "sleep") std::this_thread::sleep_for(std::chrono::milliseconds(300));
            if (op == "probe")
                answer(cannedProbe(id, line, mode));
            else if (op == "thumbnail")
                answerThumbnail(id, line, mode);
            else
                answer("{\"id\":" + id + ",\"ok\":true,\"result\":{}}");
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "--version") {
        const std::string log = envOr("DDB_FAKE_GDAL_VERSION_LOG", "");
        if (!log.empty()) appendToLog(log, "--version");
        std::printf("gdal-util fake protocol %s\n", envOr("DDB_FAKE_GDAL_PROTOCOL", "1.0").c_str());
        return 0;
    }
    if (mode == "--serve") return serve();
    std::fprintf(stderr, "usage: ddbtest_fake_gdal --version | --serve\n");
    return 64;
}
