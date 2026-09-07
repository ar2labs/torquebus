// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/diagnostics/UdsServiceCatalog.h"

#include "core/diagnostics/DiagnosticEvent.h"

#include <array>
#include <format>

namespace torquebus {
namespace {

// --- The forms ------------------------------------------------------------
//
// Each service's fields, in the order they appear in the request. The order is
// the layout: buildRequest() walks these and appends, so a field moved here is
// a byte moved on the wire.

constexpr std::array<UdsField, 1> kSessionFields{{
    {"Session", UdsField::Kind::SubFunction,
     "Which session to move the ECU into. Most services beyond reading need "
     "the extended one.",
     "03", false},
}};

constexpr std::array<UdsChoice, 4> kSessionChoices{{
    {0x01, "Default"},
    {0x02, "Programming"},
    {0x03, "Extended diagnostic"},
    {0x04, "Safety system"},
}};

constexpr std::array<UdsField, 1> kResetFields{{
    {"Reset type", UdsField::Kind::SubFunction,
     "What kind of restart to ask for.", "01", false},
}};

constexpr std::array<UdsChoice, 3> kResetChoices{{
    {0x01, "Hard reset"},
    {0x02, "Key off / on"},
    {0x03, "Soft reset"},
}};

constexpr std::array<UdsField, 1> kReadDidFields{{
    {"Identifier", UdsField::Kind::Identifier,
     "Two bytes, big-endian. F190 is the VIN on almost every ECU; F187 is the "
     "part number.",
     "F190", false},
}};

constexpr std::array<UdsField, 2> kWriteDidFields{{
    {"Identifier", UdsField::Kind::Identifier,
     "Two bytes, big-endian - the same identifier a read would use.", "", false},
    {"Value", UdsField::Kind::Bytes,
     "The bytes to write. What they mean is the ECU's business; this sends "
     "them as given.",
     "", false},
}};

constexpr std::array<UdsField, 2> kReadDtcFields{{
    {"Report type", UdsField::Kind::SubFunction,
     "Which view of the trouble codes to ask for.", "02", false},
    {"Status mask", UdsField::Kind::Byte,
     "Which codes to include. FF is all of them, 08 is confirmed only.", "FF", false},
}};

constexpr std::array<UdsChoice, 3> kReadDtcChoices{{
    {0x01, "Number of DTCs by status mask"},
    {0x02, "DTCs by status mask"},
    {0x0A, "All supported DTCs"},
}};

constexpr std::array<UdsField, 1> kClearFields{{
    {"Group", UdsField::Kind::Group,
     "Three bytes. FFFFFF clears everything the ECU stores.", "FFFFFF", false},
}};

constexpr std::array<UdsField, 2> kSecurityFields{{
    {"Level", UdsField::Kind::SubFunction,
     "Odd numbers ask for a seed; the even one after sends the key back. "
     "Which level unlocks what is the manufacturer's business.",
     "01", false},
    {"Key", UdsField::Kind::Bytes,
     "Left empty this asks for a seed. Filled in, it sends a key - and the "
     "level is used as given, so a key goes on the even number.",
     "", true},
}};

constexpr std::array<UdsField, 3> kRoutineFields{{
    {"Control", UdsField::Kind::SubFunction, "Start, stop, or ask for a result.",
     "01", false},
    {"Routine", UdsField::Kind::Identifier,
     "Two bytes, big-endian, identifying the routine.", "", false},
    {"Parameters", UdsField::Kind::Bytes, "Whatever the routine takes, if anything.",
     "", true},
}};

constexpr std::array<UdsChoice, 3> kRoutineChoices{{
    {0x01, "Start"},
    {0x02, "Stop"},
    {0x03, "Request results"},
}};

constexpr std::array<UdsField, 1> kTesterFields{{
    {"Answer", UdsField::Kind::SubFunction,
     "80 asks the ECU not to reply, which is how a heartbeat is normally sent.",
     "80", false},
}};

constexpr std::array<UdsChoice, 2> kTesterChoices{{
    {0x00, "Reply"},
    {0x80, "Do not reply"},
}};

constexpr std::array<UdsServiceTemplate, 9> kTemplates{{
    {0x22, "Read Data By Identifier",
     "Ask the ECU for the value behind an identifier.", kReadDidFields, {}},

    {0x19, "Read DTC Information", "Ask what faults the ECU has stored.",
     kReadDtcFields, kReadDtcChoices},

    {0x10, "Diagnostic Session Control",
     "Move the ECU into a session that allows more than reading.", kSessionFields,
     kSessionChoices},

    {0x2E, "Write Data By Identifier", "Write a value behind an identifier.",
     kWriteDidFields, {}},

    {0x14, "Clear Diagnostic Information", "Erase stored faults.", kClearFields, {}},

    {0x11, "ECU Reset", "Restart the ECU.", kResetFields, kResetChoices},

    {0x27, "Security Access",
     "Unlock the ECU: ask for a seed, then send the key computed from it.",
     kSecurityFields, {}},

    {0x31, "Routine Control", "Start or stop something the ECU can do on request.",
     kRoutineFields, kRoutineChoices},

    {0x3E, "Tester Present", "Keep a session from expiring.", kTesterFields,
     kTesterChoices},
}};

/// The bytes of one field's text, and how many there must be.
[[nodiscard]] Result readBytes(const UdsField& field,
                               const std::string& text,
                               std::size_t expected,
                               std::vector<std::uint8_t>& out)
{
    std::vector<std::uint8_t> bytes;

    if (!parseHexBytes(text, bytes)) {
        return Result::error(
            ErrorCode::InvalidArgument,
            std::format("{}: '{}' is not a whole number of hex bytes.", field.name, text));
    }

    // Said as a count of bytes rather than of characters, because that is the
    // unit the field is in: "this field takes two" is actionable where "expected
    // four characters" invites counting spaces.
    if (expected > 0 && bytes.size() != expected) {
        return Result::error(
            ErrorCode::InvalidArgument,
            std::format("{}: {} byte(s) given, {} needed.", field.name, bytes.size(),
                        expected));
    }

    out.insert(out.end(), bytes.begin(), bytes.end());
    return Result::ok();
}

} // namespace

std::span<const UdsServiceTemplate> serviceTemplates() noexcept
{
    return kTemplates;
}

const UdsServiceTemplate* templateFor(std::uint8_t service) noexcept
{
    for (const UdsServiceTemplate& candidate : kTemplates) {
        if (candidate.service == service) {
            return &candidate;
        }
    }

    // Not an error: a service without a form is still sendable as hex, and a
    // manufacturer-specific one always will be.
    return nullptr;
}

Result buildRequest(const UdsServiceTemplate& service,
                    std::span<const std::string> values,
                    std::vector<std::uint8_t>& out)
{
    out.clear();
    out.push_back(service.service);

    for (std::size_t index = 0; index < service.fields.size(); ++index) {
        const UdsField& field = service.fields[index];

        const std::string text =
            index < values.size() ? values[index] : std::string{field.initial};

        if (text.empty()) {
            if (field.optional) {
                continue;
            }

            return Result::error(ErrorCode::InvalidArgument,
                                 std::format("{} is needed.", field.name));
        }

        // The whole point of the file: how many bytes this kind of field is,
        // written once. An identifier is two, big-endian - which is what makes
        // typing 90 F1 impossible rather than merely wrong.
        const std::size_t expected = [&field]() -> std::size_t {
            switch (field.kind) {
            case UdsField::Kind::SubFunction:
            case UdsField::Kind::Byte:
                return 1;
            case UdsField::Kind::Identifier:
                return 2;
            case UdsField::Kind::Group:
                return 3;
            case UdsField::Kind::Bytes:
                return 0; // Any length.
            }
            return 0;
        }();

        if (Result result = readBytes(field, text, expected, out); result.failed()) {
            return result;
        }
    }

    return Result::ok();
}

} // namespace torquebus
