// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/diagnostics/DiagnosticEvent.h"

#include <format>

namespace torquebus {
namespace {

/// The value of one hex digit, or -1.
[[nodiscard]] int hexValue(char character) noexcept
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

} // namespace

bool parseHexBytes(std::string_view text, std::vector<std::uint8_t>& out)
{
    out.clear();

    // Every non-digit is a separator. That accepts "22 F1 90", "22,F1,90" and
    // "22-F1-90" without a rule for each - but it also means an `x` is a
    // separator, so "0x22 0xF1" reads as 0, 22, 0, F1... which is why the `0`
    // of an 0x prefix has to be dropped as well.
    std::string digits;
    digits.reserve(text.size());

    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];

        if ((character == 'x' || character == 'X') && !digits.empty() && digits.back() == '0') {
            // An 0x prefix: the zero already gathered was part of it.
            digits.pop_back();
            continue;
        }

        if (hexValue(character) >= 0) {
            digits.push_back(character);
        }
    }

    if (digits.empty() || (digits.size() % 2) != 0) {
        // An odd digit count is the one mistake guessing cannot resolve:
        // "2 2F 19 0" and "22 F1 90" are different requests, and picking either
        // one silently sends something the user did not write.
        return false;
    }

    out.reserve(digits.size() / 2);

    for (std::size_t index = 0; index + 1 < digits.size(); index += 2) {
        const int high = hexValue(digits[index]);
        const int low = hexValue(digits[index + 1]);

        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }

    return true;
}

std::string toHexBytes(const std::vector<std::uint8_t>& bytes, std::size_t maximum)
{
    std::string result;
    result.reserve(std::min(bytes.size(), maximum) * 3);

    for (std::size_t index = 0; index < bytes.size() && index < maximum; ++index) {
        if (index > 0) {
            result.push_back(' ');
        }
        result += std::format("{:02X}", bytes[index]);
    }

    if (bytes.size() > maximum) {
        // Said rather than silently cut: a log line that stops mid-message
        // without saying so is one somebody will quote as the whole response.
        result += std::format(" ... ({} bytes)", bytes.size());
    }

    return result;
}

} // namespace torquebus
