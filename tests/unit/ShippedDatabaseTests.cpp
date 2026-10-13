// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The databases that ship in examples/databases, held to what any database has to be.
//
// They are the first thing somebody trying the tool opens, and the ones every decoder and
// every rest bus in the examples are built on. A signal that does not fit the message it
// is declared in decodes to zero and says nothing; two signals on the same bits read each
// other's value; a bit position that is off by one is a number that is plausible and wrong.
// None of that shows up as an error. So each message and signal in each file is checked
// against the rules a database keeps, and every signal is written and read back at the
// ends of its range to prove that the bits it names are the bits it uses.

#include "core/database/CanMessage.h"
#include "core/database/DbcParser.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace torquebus;

namespace {

[[nodiscard]] std::vector<std::filesystem::path> shippedDatabases()
{
    std::vector<std::filesystem::path> files;

    for (const auto& entry : std::filesystem::directory_iterator{TORQUEBUS_EXAMPLE_DATABASE_DIR}) {
        if (entry.path().extension() == ".dbc") {
            files.push_back(entry.path());
        }
    }

    std::ranges::sort(files);
    return files;
}

/// The bits of the payload a signal occupies, as wire positions.
[[nodiscard]] std::vector<std::uint16_t> bitsOf(const CanSignal& signal)
{
    std::vector<std::uint16_t> bits;

    for (std::uint16_t index = 0; index < signal.bitLength; ++index) {
        if (signal.byteOrder == ByteOrder::Intel) {
            bits.push_back(static_cast<std::uint16_t>(signal.startBit + index));
            continue;
        }

        // Motorola: walk down from the most significant bit, as the file numbers them.
        const std::uint16_t msbWire =
            static_cast<std::uint16_t>((signal.startBit / 8U) * 8U + (7U - (signal.startBit % 8U)));
        const std::uint16_t wire = static_cast<std::uint16_t>(msbWire + index);
        bits.push_back(static_cast<std::uint16_t>((wire / 8U) * 8U + (7U - (wire % 8U))));
    }

    return bits;
}

} // namespace

TEST(ShippedDatabaseTests, TheExamplesAreThere)
{
    // A test that loops over a folder passes when the folder is empty.
    EXPECT_GE(shippedDatabases().size(), 3U);
}

TEST(ShippedDatabaseTests, EveryShippedDatabaseParses)
{
    for (const std::filesystem::path& file : shippedDatabases()) {
        CanDatabase database;
        const Result result = DbcParser::parseFile(file.string(), database);

        EXPECT_TRUE(result.succeeded()) << file.filename().string() << ": " << result.message();
        EXPECT_GT(database.messageCount(), 0U) << file.filename().string();
    }
}

TEST(ShippedDatabaseTests, EveryMessageAndSignalIsWhatADatabaseHasToBe)
{
    for (const std::filesystem::path& file : shippedDatabases()) {
        CanDatabase database;
        ASSERT_TRUE(DbcParser::parseFile(file.string(), database).succeeded());

        for (const CanMessage& message : database.messages()) {
            const std::string where = file.filename().string() + " / " + message.name;

            EXPECT_LE(message.length, kMaxCanPayload) << where;
            EXPECT_TRUE(isValidIdentifier(message.identifier, message.format)) << where;
            EXPECT_FALSE(message.signalList.empty()) << where << " has no signals";

            for (const CanSignal& signal : message.signalList) {
                const std::string name = where + "." + signal.name;

                EXPECT_GE(signal.bitLength, 1) << name;
                EXPECT_LE(signal.bitLength, 64) << name;
                EXPECT_NE(signal.factor, 0.0)
                    << name << ": a factor of zero decodes everything to the offset";
                EXPECT_TRUE(signal.fitsIn(message.length))
                    << name << " is declared past the end of its " << int{message.length}
                    << "-byte message, so it always decodes to zero";
                EXPECT_LE(signal.minimum, signal.maximum) << name << ": its range is backwards";
            }
        }
    }
}

TEST(ShippedDatabaseTests, NoTwoSignalsOfAMessageShareABit)
{
    // Multiplexed signals share bits on purpose - that is what a multiplexer is - so only
    // signals that are always present are compared with each other, and signals under the
    // same selector value with each other.
    for (const std::filesystem::path& file : shippedDatabases()) {
        CanDatabase database;
        ASSERT_TRUE(DbcParser::parseFile(file.string(), database).succeeded());

        for (const CanMessage& message : database.messages()) {
            std::map<std::int64_t, std::map<std::uint16_t, std::string>> taken;

            for (const CanSignal& signal : message.signalList) {
                // -1 for the always-present group; the selector value otherwise.
                const std::int64_t group = signal.multiplexerValue.has_value()
                                               ? static_cast<std::int64_t>(*signal.multiplexerValue)
                                               : -1;

                for (const std::uint16_t bit : bitsOf(signal)) {
                    // A multiplexed signal also must not overlap an always-present one.
                    for (const std::int64_t other : {std::int64_t{-1}, group}) {
                        const auto found = taken[other].find(bit);
                        EXPECT_TRUE(found == taken[other].end())
                            << file.filename().string() << " / " << message.name << ": "
                            << signal.name << " and "
                            << (found != taken[other].end() ? found->second : "")
                            << " both use bit " << bit;
                    }
                }

                for (const std::uint16_t bit : bitsOf(signal)) {
                    taken[group][bit] = signal.name;
                }
            }
        }
    }
}

TEST(ShippedDatabaseTests, EverySignalIsReadBackFromWhereItWasWritten)
{
    // The bits a signal names are the bits it uses: written at the bottom, the middle and the
    // top of its raw range, and read back unchanged. A start bit off by one, a byte order
    // mistaken, or a length that is not the one the file says would each fail here for the
    // signal concerned - and for no other, because the rest of the payload is checked to be
    // untouched.
    for (const std::filesystem::path& file : shippedDatabases()) {
        CanDatabase database;
        ASSERT_TRUE(DbcParser::parseFile(file.string(), database).succeeded());

        for (const CanMessage& message : database.messages()) {
            for (const CanSignal& signal : message.signalList) {
                const std::string name =
                    file.filename().string() + " / " + message.name + "." + signal.name;

                if (!signal.fitsIn(message.length)) {
                    continue; // Reported by the test above; nothing to write into.
                }

                const std::int64_t low = signal.minimumRaw();
                const std::int64_t high = signal.maximumRaw();

                for (const std::int64_t raw : {low, low / 2 + high / 2, high}) {
                    std::vector<std::uint8_t> payload(message.length, 0xA5);
                    const std::vector<std::uint8_t> untouched = payload;

                    ASSERT_TRUE(signal.encodeRaw(raw, payload.data(), payload.size())) << name;
                    EXPECT_EQ(signal.rawValue(payload.data(), payload.size()), raw) << name;

                    // Only this signal's bits changed.
                    const std::vector<std::uint16_t> mine = bitsOf(signal);

                    for (std::size_t byte = 0; byte < payload.size(); ++byte) {
                        for (std::uint16_t bit = 0; bit < 8; ++bit) {
                            const auto position = static_cast<std::uint16_t>(byte * 8U + bit);
                            if (std::ranges::find(mine, position) != mine.end()) {
                                continue;
                            }

                            EXPECT_EQ((payload[byte] >> bit) & 1, (untouched[byte] >> bit) & 1)
                                << name << " wrote bit " << position << ", which is not its own";
                        }
                    }
                }
            }
        }
    }
}
