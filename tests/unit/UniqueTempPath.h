// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A temporary path that is unique across processes, not only within one.
//
// catch_discover_tests registers every TEST_CASE as its own CTest test, so each
// case runs in a fresh process and any function-local counter restarts at one.
// Serially that is harmless - a case finishes and cleans up before the next one
// starts. Under `ctest -j` it is not: two cases run at the same time, pick the
// same name in TEMP, and delete each other's file mid-test. The failure moves
// around, never reproduces in isolation, and reads like a bug in the code under
// test. It is not; it is the fixture.
//
// The per-process tag comes from random_device rather than a process id so this
// stays standard C++ with no platform header.

#pragma once

#include <cstdint>
#include <filesystem>
#include <random>
#include <sstream>
#include <string>

namespace torquebus::tests {

/// Sixteen hex digits, drawn once per process.
[[nodiscard]] inline const std::string& processTag()
{
    static const std::string tag = [] {
        std::random_device source;
        std::ostringstream out;

        out << std::hex
            << ((static_cast<std::uint64_t>(source()) << 32)
                | static_cast<std::uint64_t>(source()));

        return out.str();
    }();

    return tag;
}

/// `<temp>/<stem>_<tag>_<serial><extension>`, where `extension` carries its own
/// dot. The serial keeps names apart inside one process, the tag across them.
[[nodiscard]] inline std::filesystem::path uniqueTempPath(const std::string& stem,
                                                          int serial,
                                                          const std::string& extension)
{
    return std::filesystem::temp_directory_path()
           / (stem + "_" + processTag() + "_" + std::to_string(serial) + extension);
}

/// `<temp>/<stem>_<tag>`, for a fixture that owns a whole directory.
[[nodiscard]] inline std::filesystem::path uniqueTempDirectory(const std::string& stem)
{
    return std::filesystem::temp_directory_path() / (stem + "_" + processTag());
}

}  // namespace torquebus::tests
