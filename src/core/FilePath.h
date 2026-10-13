// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A path that is text, turned into a path the platform can open.
//
// Every string in TorqueBus is UTF-8: Qt's toStdString() is, the project file is, and
// so is what a person types into a "Database" field. On Linux and macOS a UTF-8
// string is also what the filesystem takes, and nothing needs doing. On Windows it
// is not. A narrow path handed to std::ifstream or std::filesystem::path is read in
// the ANSI code page - Windows-1252 on a Brazilian machine - so "C:\Users\João" is
// opened as "C:\Users\JoÃ£o", which does not exist, and the file is reported as
// missing from a folder the person is looking at.
//
// It is not an exotic case. "Área de Trabalho", "Usuários", a project folder called
// "Simulação" - every one of them put a database, a script or a log out of reach of
// the core while the window opened them without trouble, because the window goes
// through Qt, which converts properly.
//
// So the core converts at the one place it can: here, where the bytes are known to be
// UTF-8, and not at each of the half dozen sites that open a file.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace torquebus {

/// The path a UTF-8 string names. An invalid sequence does not throw; it becomes a
/// path that does not exist, which is what it is.
[[nodiscard]] inline std::filesystem::path pathFromUtf8(std::string_view utf8) noexcept
{
    // C++20 gives a char8_t overload that is documented to be read as UTF-8 on every
    // platform; the cast is the only way to reach it from a std::string.
    //
    // The conversion on Windows is a system call, and it reports bytes that are not
    // UTF-8 by throwing. A path somebody typed wrongly is not worth ending the
    // program over, and an empty path fails to open with the message the caller was
    // going to give anyway.
    try {
        return std::filesystem::path{
            std::u8string{reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()}};
    } catch (...) {
        return {};
    }
}

/// The same path as UTF-8 text, for putting it into a message or a project file.
/// path::string() is not this: on Windows it is the ANSI code page, and replaces
/// what that cannot hold with a question mark.
[[nodiscard]] inline std::string utf8FromPath(const std::filesystem::path& path)
{
    try {
        const std::u8string text = path.u8string();
        return std::string{reinterpret_cast<const char*>(text.data()), text.size()};
    } catch (...) {
        // A name Windows allows and UTF-8 cannot hold, such as a lone surrogate. The
        // lossy form is still something to show.
        return path.string();
    }
}

} // namespace torquebus
