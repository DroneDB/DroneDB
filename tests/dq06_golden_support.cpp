/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "dq06_golden_support.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>

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
