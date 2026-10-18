// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/dashboard/DashboardAssets.h"

#include "core/FilePath.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace torquebus {
namespace {

namespace fs = std::filesystem;

/// A path as stored: normalised, forward slashes, UTF-8. Not utf8FromPath, which is the platform's
/// own form and so has backslashes on Windows - and a project is opened on more than one platform.
[[nodiscard]] std::string textOf(const fs::path& path)
{
    try {
        const std::u8string text = path.lexically_normal().generic_u8string();
        return std::string{reinterpret_cast<const char*>(text.data()), text.size()};
    } catch (...) {
        // A name Windows allows and UTF-8 cannot hold. The lossy form is still something to show.
        return path.generic_string();
    }
}

[[nodiscard]] std::string lowered(std::string_view text)
{
    std::string result{text};

    for (char& c : result) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }

    return result;
}

/// How far up a relative path goes: the number of leading `..`.
[[nodiscard]] std::size_t levelsUp(const fs::path& relative)
{
    std::size_t levels = 0;

    for (const fs::path& part : relative) {
        if (part == "..") {
            ++levels;
        } else {
            break;
        }
    }

    return levels;
}

/// The widget's own path and each state's, in that order, skipping the empty ones.
template<typename Visitor>
void forEachImage(DashboardWidget& widget, Visitor&& visit)
{
    if (!widget.image.empty()) {
        visit(widget.image);
    }

    for (DashboardState& state : widget.states) {
        if (!state.image.empty()) {
            visit(state.image);
        }
    }
}

} // namespace

const std::vector<std::string_view>& imageExtensions()
{
    static const std::vector<std::string_view> kExtensions{
        "png", "jpg", "jpeg", "bmp", "gif", "svg"};

    return kExtensions;
}

bool isImagePath(std::string_view path)
{
    const std::size_t dot = path.rfind('.');

    if (dot == std::string_view::npos || dot + 1 >= path.size()) {
        return false;
    }

    // A dot in a folder name is not an extension: "a.b/picture" has none.
    if (path.find_first_of("/\\", dot) != std::string_view::npos) {
        return false;
    }

    const std::string extension = lowered(path.substr(dot + 1));
    const std::vector<std::string_view>& known = imageExtensions();

    return std::find(known.begin(), known.end(), extension) != known.end();
}

std::string resolveAsset(std::string_view image, std::string_view baseDir)
{
    if (image.empty()) {
        return {};
    }

    fs::path path = pathFromUtf8(image);

    if (!path.is_absolute()) {
        std::error_code error;

        // The project's folder; or, for a project that has none, where the program is running -
        // the same answer a script path gets.
        const fs::path base = baseDir.empty() ? fs::current_path(error) : pathFromUtf8(baseDir);

        // An absolute() that fails leaves the path as it was: better a path that does not exist
        // than none.
        path = fs::absolute(base / path, error);

        if (error) {
            path = base / pathFromUtf8(image);
        }
    }

    return textOf(path);
}

std::string storableAsset(std::string_view absolute, std::string_view baseDir)
{
    if (absolute.empty()) {
        return {};
    }

    const fs::path whole = pathFromUtf8(absolute).lexically_normal();

    if (baseDir.empty()) {
        return textOf(whole);
    }

    const fs::path base = pathFromUtf8(baseDir).lexically_normal();

    // Empty when the two have no common root - another drive - and that is the answer.
    const fs::path relative = whole.lexically_relative(base);

    constexpr std::size_t kMostLevelsUp = 4;

    if (relative.empty() || relative.is_absolute() || levelsUp(relative) > kMostLevelsUp) {
        return textOf(whole);
    }

    return textOf(relative);
}

std::vector<std::string> assetsUsedBy(const DashboardDescription& dashboard)
{
    std::vector<std::string> used;

    const auto note = [&used](const std::string& image) {
        if (std::find(used.begin(), used.end(), image) == used.end()) {
            used.push_back(image);
        }
    };

    for (const DashboardWidget& widget : dashboard.widgets()) {
        if (!widget.image.empty()) {
            note(widget.image);
        }

        for (const DashboardState& state : widget.states) {
            if (!state.image.empty()) {
                note(state.image);
            }
        }
    }

    return used;
}

void rebaseAssets(DashboardDescription& dashboard, std::string_view fromDir, std::string_view toDir)
{
    if (fromDir == toDir) {
        return;
    }

    for (DashboardWidget& widget : dashboard.widgets()) {
        forEachImage(widget, [fromDir, toDir](std::string& image) {
            image = storableAsset(resolveAsset(image, fromDir), toDir);
        });
    }
}

std::vector<MissingAsset> missingAssets(const DashboardDescription& dashboard,
                                        std::string_view baseDir)
{
    std::vector<MissingAsset> missing;

    const auto check = [&missing, baseDir](const std::string& widgetId, const std::string& image) {
        std::error_code error;

        if (fs::exists(pathFromUtf8(resolveAsset(image, baseDir)), error) && !error) {
            return;
        }

        const auto already = std::find_if(
            missing.begin(), missing.end(), [&widgetId, &image](const MissingAsset& found) {
                return found.widgetId == widgetId && found.image == image;
            });

        if (already == missing.end()) {
            missing.push_back({widgetId, image});
        }
    };

    for (const DashboardWidget& widget : dashboard.widgets()) {
        if (!widget.image.empty()) {
            check(widget.id, widget.image);
        }

        for (const DashboardState& state : widget.states) {
            if (!state.image.empty()) {
                check(widget.id, state.image);
            }
        }
    }

    return missing;
}

} // namespace torquebus
