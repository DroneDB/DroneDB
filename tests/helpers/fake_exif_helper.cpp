/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// A stand-in for metadata-util (vendor/metadata-util/PROTOCOL.md) for the tests of the metadata pool. It
// answers `--version` and `--serve` like the real program, and breaks the way the file name of a
// request tells it to, so one helper can serve good and bad requests in any order:
//
//   <name>.hold         waits until the file <name>.release exists, then answers (keeps the
//                       helper busy as long as the test wants)
//   crash.*             exits in the middle of the request, without answering
//   hang.*              never answers
//   garbage.*           answers a line that is not JSON
//   wrongid.*           answers another request's id
//   huge.*              answers a line of 20 MiB
//   rejected.*          answers error "request", as for a malformed request
//   noimage.*           answers ok: false, error "exiv2", as for a file that is not an image
//   missing.*           answers ok: false, error "open"
//   anything else       a good answer: a few tags, one of which carries the name of the file
//
// DDB_FAKE_EXIF_PROTOCOL ("2.0") sets the protocol `--version` reports (default "1.0").
// DDB_FAKE_EXIF_LOG names a file where every request is appended as "<pid> <file name>".
// DDB_FAKE_EXIF_VERSION_LOG names a file where every run of `--version` appends a line.
// DDB_FAKE_EXIF_VERSION_DELAY_MS makes `--version` take that long to answer, after its line in
// the version log (a check that is still running when other helpers of the program start).
// DDB_FAKE_EXIF_EXIT_ON_EOF_DELAY_MS makes the helper wait that long after its input ended
// before it exits (a helper that does not end by itself within 2 s is killed).
// DDB_FAKE_EXIF_EXIT_STATUS makes the helper end with that exit code once its input ended, after
// a line on stderr, as a helper built with LeakSanitizer does when it finds a leak at exit.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <process.h>
#define FAKE_GETPID _getpid
#else
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

// Text as the inside of a JSON string
std::string escaped(const std::string &text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

bool startsWith(const std::string &text, const char *prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool exists(const std::string &path) {
    return std::ifstream(path).good();
}

std::string tag(const std::string &key, const std::string &type, size_t count, const std::string &text,
                const std::string &components) {
    return "{\"k\":\"" + key + "\",\"t\":\"" + type + "\",\"n\":" + std::to_string(count) + ",\"s\":\"" + text +
           "\",\"v\":[" + components + "]}";
}

std::string goodAnswer(const std::string &id, const std::string &line, const std::string &path) {
    // The file name as the request spelled it, or its base64 when the client sent path64
    std::string shown = path;
    if (shown.empty()) shown = "path64:" + stringMember(line, "path64");
    std::string exif =
        tag("Exif.Image.Make", "Ascii", 4, "DJI", "[68,68.0,[68,1],true],[74,74.0,[74,1],true],[73,73.0,[73,1],true],[0,0.0,[0,1],true]") +
        "," + tag("Exif.Image.ImageDescription", "Ascii", shown.size() + 1, escaped(shown), "[0,0.0,[0,1],true],null,null") +
        "," + tag("Exif.GPSInfo.GPSLatitude", "Rational", 3, "46/1 4/1 5129/1000",
                  "[46,46.0,[46,1],true],[4,4.0,[4,1],true],[5,5.129,[5129,1000],true]");
    std::string xmp = tag("Xmp.drone-dji.GimbalYawDegree", "XmpText", 6, "+12.50",
                          "[12,12.5,[12,1],true],[12,12.5,[12,1],true],[12,12.5,[12,1],true]");
    return "{\"id\":" + id + ",\"ok\":true,\"width\":4000,\"height\":3000,\"exif\":[" + exif + "],\"xmp\":[" + xmp +
           "]}";
}

void answer(const std::string &text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int serve() {
    const std::string log = envOr("DDB_FAKE_EXIF_LOG", "");
    std::string line;
    while (std::getline(std::cin, line)) {
        const std::string id = idOf(line);
        const std::string path = stringMember(line, "path");
        const std::string name = baseName(path);

        if (!log.empty()) {
            std::ofstream out(log, std::ios::app);
            out << FAKE_GETPID() << " " << name << "\n";
        }

        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".hold") == 0) {
            while (!exists(path + ".release")) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            answer(goodAnswer(id, line, path));
        } else if (startsWith(name, "crash")) {
            std::_Exit(7);
        } else if (startsWith(name, "hang")) {
            for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
        } else if (startsWith(name, "garbage")) {
            answer("this is not json");
        } else if (startsWith(name, "wrongid")) {
            answer("{\"id\":987654321,\"ok\":true,\"width\":1,\"height\":1,\"exif\":[],\"xmp\":[]}");
        } else if (startsWith(name, "huge")) {
            const std::string chunk(1024 * 1024, 'x');
            std::fputs("{\"id\":", stdout);
            for (int i = 0; i < 20; i++) std::fwrite(chunk.data(), 1, chunk.size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        } else if (startsWith(name, "rejected")) {
            answer("{\"id\":" + id + ",\"ok\":false,\"error\":\"request\",\"message\":\"not a valid request\"}");
        } else if (startsWith(name, "noimage")) {
            answer("{\"id\":" + id + ",\"ok\":false,\"error\":\"exiv2\",\"message\":\"not an image\"}");
        } else if (startsWith(name, "missing")) {
            answer("{\"id\":" + id + ",\"ok\":false,\"error\":\"open\",\"message\":\"cannot open\"}");
        } else if (line.find("\"op\":\"write\"") != std::string::npos) {
            answer("{\"id\":" + id + ",\"ok\":true,\"tags\":[{\"k\":\"Exif.Image.Make\",\"t\":\"Ascii\",\"n\":4,\"s\":\"DJI\"}]}");
        } else {
            answer(goodAnswer(id, line, path));
        }
    }
    const int delayMs = std::atoi(envOr("DDB_FAKE_EXIF_EXIT_ON_EOF_DELAY_MS", "0").c_str());
    if (delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    const int status = std::atoi(envOr("DDB_FAKE_EXIF_EXIT_STATUS", "0").c_str());
    if (status != 0) {
        std::fputs("ddbtest_fake_exif: a leak report at exit\n", stderr);
        std::fflush(stderr);
    }
    return status;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "--version") {
        const std::string log = envOr("DDB_FAKE_EXIF_VERSION_LOG", "");
        if (!log.empty()) std::ofstream(log, std::ios::app) << FAKE_GETPID() << "\n";
        const int delayMs = std::atoi(envOr("DDB_FAKE_EXIF_VERSION_DELAY_MS", "0").c_str());
        if (delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        std::printf("metadata-util fake (Exiv2 0.0.0) protocol %s\n", envOr("DDB_FAKE_EXIF_PROTOCOL", "1.0").c_str());
        return 0;
    }
    if (mode == "--serve") return serve();
    std::fprintf(stderr, "usage: ddbtest_fake_exif --version | --serve\n");
    return 64;
}
