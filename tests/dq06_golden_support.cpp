/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "dq06_golden_support.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <vector>

#include <zlib.h>

#include "gtest/gtest.h"

namespace dq06
{
    namespace
    {
        fs::path goldenPath(const std::string &name)
        {
            return fs::path(DDB_DQ06_GOLDEN_DIR) / (name + ".json");
        }

        // Keys whose values change on every run
        bool isVolatileKey(const std::string &key)
        {
            return key == "computedAt" || key == "calculatedAt" || key == "timestamp" || key == "generatedAt";
        }

        void dropVolatile(json &j)
        {
            if (j.is_object())
            {
                for (auto it = j.begin(); it != j.end();)
                {
                    if (isVolatileKey(it.key()))
                        it = j.erase(it);
                    else
                    {
                        dropVolatile(it.value());
                        ++it;
                    }
                }
            }
            else if (j.is_array())
            {
                for (auto &item : j)
                    dropVolatile(item);
            }
        }

        void writeText(const fs::path &path, const std::string &text)
        {
            fs::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::binary);
            out << text;
        }
    }

    bool jsonNear(const json &a, const json &b, std::string &where, const std::string &at)
    {
        if (a.is_number() && b.is_number())
        {
            const double x = a.get<double>(), y = b.get<double>();
            if (std::isnan(x) && std::isnan(y))
                return true;
            const double tolerance = 1e-9 * std::max({1.0, std::fabs(x), std::fabs(y)});
            if (std::fabs(x - y) <= tolerance)
                return true;
            where = at + ": " + a.dump() + " != " + b.dump();
            return false;
        }
        if (a.type() != b.type())
        {
            where = at + ": type " + a.dump() + " != " + b.dump();
            return false;
        }
        if (a.is_object())
        {
            if (a.size() != b.size())
            {
                where = at + ": keys differ (" + std::to_string(a.size()) + " vs " + std::to_string(b.size()) + ")";
                return false;
            }
            for (auto it = a.begin(); it != a.end(); ++it)
            {
                if (!b.contains(it.key()))
                {
                    where = at + "/" + it.key() + ": missing in the golden";
                    return false;
                }
                if (!jsonNear(it.value(), b.at(it.key()), where, at + "/" + it.key()))
                    return false;
            }
            return true;
        }
        if (a.is_array())
        {
            if (a.size() != b.size())
            {
                where = at + ": length " + std::to_string(a.size()) + " != " + std::to_string(b.size());
                return false;
            }
            for (size_t i = 0; i < a.size(); i++)
                if (!jsonNear(a[i], b[i], where, at + "/" + std::to_string(i)))
                    return false;
            return true;
        }
        if (a != b)
        {
            where = at + ": " + a.dump() + " != " + b.dump();
            return false;
        }
        return true;
    }

    void dumpOnly(const std::string &name, const json &value)
    {
        if (const char *dir = std::getenv("DDB_DQ06_DUMP_DIR"))
            writeText(fs::path(dir) / (name + ".json"), value.dump());
    }

    void expectGolden(const std::string &name, json actual)
    {
        dropVolatile(actual);

        if (const char *dir = std::getenv("DDB_DQ06_DUMP_DIR"))
            writeText(fs::path(dir) / (name + ".json"), actual.dump());

        const fs::path path = goldenPath(name);
        if (const char *update = std::getenv("DDB_UPDATE_DQ06_GOLDEN"); update != nullptr && std::string(update) == "1")
        {
            writeText(path, actual.dump(1) + "\n");
            return;
        }

        std::ifstream in(path, std::ios::binary);
        ASSERT_TRUE(in.good()) << "No golden " << path << " (DDB_UPDATE_DQ06_GOLDEN=1 writes it)";
        std::stringstream text;
        text << in.rdbuf();
        const json golden = json::parse(text.str());

        std::string where;
        EXPECT_TRUE(jsonNear(actual, golden, where)) << name << " differs from its golden at " << where;
    }

    std::string scrubFolder(std::string message, const std::string &folder)
    {
        if (folder.empty())
            return message;
        const std::string marker = "<dir>";
        for (size_t at = message.find(folder); at != std::string::npos; at = message.find(folder, at + marker.size()))
        {
            message.replace(at, folder.size(), marker);
            // The rest of that path is written with the platform's separators: the goldens use '/'
            for (size_t i = at + marker.size();
                 i < message.size() && message[i] != ' ' && message[i] != '"' && message[i] != '\'' && message[i] != ')';
                 i++)
                if (message[i] == '\\')
                    message[i] = '/';
        }
        return message;
    }

    json capture(const std::function<std::string()> &call, const std::string &scrub)
    {
        try
        {
            return json::parse(call());
        }
        catch (const std::exception &e)
        {
            std::string message = scrubFolder(e.what(), scrub);
            return json{{"error", message}};
        }
    }

    namespace
    {
        constexpr char kPixelsMagic[4] = {'D', 'Q', '6', 'P'};

        void putU32(std::string &out, uint32_t v)
        {
            for (int i = 0; i < 4; i++)
                out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        }

        uint32_t getU32(const std::string &in, size_t at)
        {
            uint32_t v = 0;
            for (int i = 0; i < 4; i++)
                v |= static_cast<uint32_t>(static_cast<uint8_t>(in[at + i])) << (8 * i);
            return v;
        }

        uint32_t bitsOf(float f)
        {
            uint32_t v;
            std::memcpy(&v, &f, sizeof(v));
            return v;
        }

        float floatOf(uint32_t v)
        {
            float f;
            std::memcpy(&f, &v, sizeof(f));
            return f;
        }

        // The float32 values, the first byte of every value first, then the second ... (the high
        // bytes of smooth data repeat, so it deflates well), after a header of band count, width
        // and height
        std::string encodePixels(int width, int height, const std::vector<std::vector<float>> &bands)
        {
            std::string raw(kPixelsMagic, sizeof(kPixelsMagic));
            putU32(raw, static_cast<uint32_t>(bands.size()));
            putU32(raw, static_cast<uint32_t>(width));
            putU32(raw, static_cast<uint32_t>(height));
            for (const auto &band : bands)
                for (int byte = 0; byte < 4; byte++)
                    for (const float f : band)
                        raw.push_back(static_cast<char>((bitsOf(f) >> (8 * byte)) & 0xFF));

            uLongf size = compressBound(static_cast<uLong>(raw.size()));
            std::string out(size, '\0');
            if (compress2(reinterpret_cast<Bytef *>(out.data()), &size, reinterpret_cast<const Bytef *>(raw.data()),
                          static_cast<uLong>(raw.size()), Z_BEST_COMPRESSION) != Z_OK)
                throw std::runtime_error("cannot deflate the pixels");
            // The size of the inflated text, for the reader
            std::string sized;
            putU32(sized, static_cast<uint32_t>(raw.size()));
            return sized + out.substr(0, size);
        }

        bool decodePixels(const std::string &file, int &width, int &height, std::vector<std::vector<float>> &bands)
        {
            if (file.size() < 4)
                return false;
            std::string raw(getU32(file, 0), '\0');
            uLongf size = static_cast<uLong>(raw.size());
            if (uncompress(reinterpret_cast<Bytef *>(raw.data()), &size, reinterpret_cast<const Bytef *>(file.data() + 4),
                           static_cast<uLong>(file.size() - 4)) != Z_OK ||
                size != raw.size() || raw.size() < 16 || raw.compare(0, 4, kPixelsMagic, 4) != 0)
                return false;
            const size_t count = getU32(raw, 4);
            width = static_cast<int>(getU32(raw, 8));
            height = static_cast<int>(getU32(raw, 12));
            const size_t pixels = static_cast<size_t>(width) * height;
            if (raw.size() != 16 + count * pixels * 4)
                return false;
            bands.assign(count, std::vector<float>(pixels));
            size_t at = 16;
            for (auto &band : bands)
                for (int byte = 0; byte < 4; byte++)
                    for (size_t i = 0; i < pixels; i++)
                    {
                        const uint32_t v = static_cast<uint8_t>(raw[at++]);
                        band[i] = floatOf(bitsOf(band[i]) | (v << (8 * byte)));
                    }
            return true;
        }
    }

    void expectPixelsNear(const std::string &name, int width, int height, const std::vector<std::vector<double>> &bands)
    {
        std::vector<std::vector<float>> actual;
        for (const auto &band : bands)
            actual.emplace_back(band.begin(), band.end());

        const fs::path path = fs::path(DDB_DQ06_GOLDEN_DIR) / "pixels" / (name + ".bin");
        if (const char *update = std::getenv("DDB_UPDATE_DQ06_GOLDEN"); update != nullptr && std::string(update) == "1")
        {
            writeText(path, encodePixels(width, height, actual));
            return;
        }

        std::ifstream in(path, std::ios::binary);
        ASSERT_TRUE(in.good()) << "No golden " << path << " (DDB_UPDATE_DQ06_GOLDEN=1 writes it)";
        std::stringstream text;
        text << in.rdbuf();
        int goldenWidth = 0, goldenHeight = 0;
        std::vector<std::vector<float>> golden;
        ASSERT_TRUE(decodePixels(text.str(), goldenWidth, goldenHeight, golden)) << "Cannot read the golden " << path;

        ASSERT_EQ(golden.size(), actual.size()) << name << ": bands";
        ASSERT_EQ(goldenWidth, width) << name << ": width";
        ASSERT_EQ(goldenHeight, height) << name << ": height";

        size_t different = 0;
        std::ostringstream first;
        for (size_t b = 0; b < actual.size(); b++)
        {
            for (size_t i = 0; i < actual[b].size(); i++)
            {
                const double a = actual[b][i], g = golden[b][i];
                const bool same = (std::isnan(a) && std::isnan(g)) ||
                                  (a == g || std::fabs(a - g) <= std::max(1e-5 * std::fabs(g), 1e-6));
                if (same)
                    continue;
                if (++different <= 5)
                    first << "\n  band " << b + 1 << " pixel (" << i % static_cast<size_t>(width) << ", "
                          << i / static_cast<size_t>(width) << "): " << a << " != " << g;
            }
        }
        EXPECT_EQ(different, 0u) << name << ": pixels differ from the golden " << path << first.str();
    }

    std::string fnv1a(const void *data, size_t size)
    {
        uint64_t h = 1469598103934665603ULL;
        const auto *p = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < size; i++)
        {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
        return buf;
    }
}
