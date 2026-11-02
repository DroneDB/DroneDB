/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <algorithm>
#include <cctype>

#include "layers.h"
#include "sandbox/sandbox.h"

namespace sandbox
{
    namespace
    {
        bool contains(const std::vector<std::string> &names, const std::string &name)
        {
            return std::find(names.begin(), names.end(), name) != names.end();
        }

        std::string lowered(const std::string &text)
        {
            std::string out = text;
            for (char &c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        std::string trimmed(const std::string &text)
        {
            const char *spaces = " \t\r\n";
            const size_t first = text.find_first_not_of(spaces);
            if (first == std::string::npos)
                return std::string();
            return text.substr(first, text.find_last_not_of(spaces) - first + 1);
        }

        std::string describeMissing(const std::vector<LayerState> &layers)
        {
            std::string text;
            for (const auto &layer : layers)
            {
                if (layer.active)
                    continue;
                if (!text.empty())
                    text += ", ";
                text += layer.name;
                if (!layer.detail.empty())
                    text += " (" + layer.detail + ")";
            }
            return text;
        }

        // The state of every layer without applying any, with the layers the policy declares
        // unavailable shown as missing
        std::vector<LayerState> probeLayers(const Policy &policy)
        {
            std::vector<LayerState> states;
            for (const auto &layer : detail::platformLayers())
                states.push_back(contains(policy.unavailableLayers, layer.name)
                                     ? detail::state(layer.name, false, "made unavailable by the policy")
                                     : layer.probe());
            return states;
        }
    } // namespace

    Mode parseMode(const char *value, std::string *warning)
    {
        if (warning != nullptr)
            warning->clear();
        if (value == nullptr)
            return Mode::BestEffort;
        const std::string text = trimmed(value);
        if (text.empty())
            return Mode::BestEffort;
        const std::string name = lowered(text);
        if (name == "strict")
            return Mode::Strict;
        if (name == "best-effort")
            return Mode::BestEffort;
        if (name == "off")
            return Mode::Off;
        if (warning != nullptr)
            *warning = "Unknown sandbox mode '" + text + "' (expected strict, best-effort or off), using best-effort";
        return Mode::BestEffort;
    }

    const char *modeName(Mode mode)
    {
        switch (mode)
        {
        case Mode::Strict:
            return "strict";
        case Mode::Off:
            return "off";
        case Mode::BestEffort:
            break;
        }
        return "best-effort";
    }

    std::vector<std::string> Report::missingLayers() const
    {
        std::vector<std::string> names;
        for (const auto &layer : layers)
            if (!layer.active)
                names.push_back(layer.name);
        return names;
    }

    std::string Report::warning() const
    {
        const std::string missing = describeMissing(layers);
        return missing.empty() ? std::string() : "Sandbox layers not available: " + missing;
    }

    std::string Report::describe() const
    {
        std::string text;
        for (const auto &layer : layers)
        {
            text += layer.name + ": " + (layer.active ? "active" : "missing");
            if (!layer.detail.empty())
                text += " (" + layer.detail + ")";
            text += "\n";
        }
        return text;
    }

    Report probe()
    {
        Report report;
        report.layers = probeLayers(Policy());
        return report;
    }

    Report apply(const Policy &policy, Mode mode)
    {
        Report report;
        report.mode = mode;
        if (mode == Mode::Off)
            return report;

        // Strict refuses before it restricts anything: a process that is going to exit has no
        // use for half a sandbox
        if (mode == Mode::Strict)
        {
            const std::string missing = describeMissing(probeLayers(policy));
            if (!missing.empty())
            {
                report.ok = false;
                report.error = "Sandbox layers not available in strict mode: " + missing;
                report.layers = probeLayers(policy);
                return report;
            }
        }

        for (const auto &layer : detail::platformLayers())
        {
            const detail::LayerResult applied =
                contains(policy.unavailableLayers, layer.name)
                    ? detail::result(layer.name, false, "made unavailable by the policy")
                    : layer.apply(policy);
            report.layers.push_back(applied.state);
            if (applied.fatal)
            {
                report.ok = false;
                report.error = std::string("Cannot apply the sandbox layer ") + layer.name + ": " + applied.state.detail;
                return report;
            }
        }

        const std::string missing = describeMissing(report.layers);
        if (mode == Mode::Strict && !missing.empty())
        {
            report.ok = false;
            report.error = "Sandbox layers not available in strict mode: " + missing;
        }
        return report;
    }
} // namespace sandbox
