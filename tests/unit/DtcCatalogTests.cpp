// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/diagnostics/DtcCatalog.h"
#include "core/diagnostics/UdsTypes.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

using namespace torquebus;

TEST(DtcCatalogTests, AnalyzesPowertrainGenericAndManufacturerSpecificStructure)
{
    // P0300 -> 0x030000
    const DtcStructure p0300 = analyzeDtcCode(0x030000U);
    EXPECT_EQ(p0300.code, "P0300");
    EXPECT_EQ(p0300.system, DtcSystem::Powertrain);
    EXPECT_EQ(p0300.scope, DtcScope::Generic);
    EXPECT_EQ(p0300.firstDigit, '0');
    EXPECT_EQ(p0300.subsystemDigit, '3');
    EXPECT_EQ(p0300.subsystemName, "Ignition System or Misfire");

    // P0171 -> 0x017100
    const DtcStructure p0171 = analyzeDtcCode(0x017100U);
    EXPECT_EQ(p0171.code, "P0171");
    EXPECT_EQ(p0171.system, DtcSystem::Powertrain);
    EXPECT_EQ(p0171.scope, DtcScope::Generic);
    EXPECT_EQ(p0171.subsystemDigit, '1');
    EXPECT_EQ(p0171.subsystemName, "Fuel and Air Metering");

    // P1128 -> 0x112800 (OEM manufacturer-specific)
    const DtcStructure p1128 = analyzeDtcCode(0x112800U);
    EXPECT_EQ(p1128.code, "P1128");
    EXPECT_EQ(p1128.system, DtcSystem::Powertrain);
    EXPECT_EQ(p1128.scope, DtcScope::ManufacturerSpecific);
    EXPECT_EQ(p1128.firstDigit, '1');
}

TEST(DtcCatalogTests, AnalyzesChassisBodyAndNetworkFamilies)
{
    // C0035 -> 0x403500
    const DtcStructure c0035 = analyzeDtcCode(0x403500U);
    EXPECT_EQ(c0035.code, "C0035");
    EXPECT_EQ(c0035.system, DtcSystem::Chassis);
    EXPECT_EQ(c0035.scope, DtcScope::Generic);

    // B1000 -> 0x900000
    const DtcStructure b1000 = analyzeDtcCode(0x900000U);
    EXPECT_EQ(b1000.code, "B1000");
    EXPECT_EQ(b1000.system, DtcSystem::Body);
    EXPECT_EQ(b1000.scope, DtcScope::ManufacturerSpecific);

    // U0100 -> 0xC10000
    const DtcStructure u0100 = analyzeDtcCode(0xC10000U);
    EXPECT_EQ(u0100.code, "U0100");
    EXPECT_EQ(u0100.system, DtcSystem::Network);
    EXPECT_EQ(u0100.scope, DtcScope::Generic);
    EXPECT_EQ(u0100.subsystemDigit, '1');
    EXPECT_EQ(u0100.subsystemName, "Lost Communication With Control Module");
}

TEST(DtcCatalogTests, ParsesFiveCharExtendedAndRawUdsHexCodes)
{
    // 5-char standard string (case-insensitive, whitespace trimmed)
    const auto p0420 = parseDtcCode("  p0420 ");
    ASSERT_TRUE(p0420.has_value());
    EXPECT_EQ(p0420->code, "P0420");
    EXPECT_EQ(p0420->rawCode, 0x042000U);
    EXPECT_EQ(p0420->subsystemName, "Auxiliary Emission Controls");

    // Extended UDS format with failure type byte ("P0128-35")
    const auto extended = parseDtcCode("P0128-35");
    ASSERT_TRUE(extended.has_value());
    EXPECT_EQ(extended->code, "P0128");
    EXPECT_EQ(extended->rawCode, 0x012835U);
    EXPECT_EQ(extended->failureType, 0x35U);
    EXPECT_TRUE(extended->hasFailureType);

    // 3-byte UDS hex ("01 28 00" or "0xC03500")
    const auto hexSpaced = parseDtcCode("01 28 00");
    ASSERT_TRUE(hexSpaced.has_value());
    EXPECT_EQ(hexSpaced->code, "P0128");
    EXPECT_EQ(hexSpaced->rawCode, 0x012800U);

    const auto hexPrefixed = parseDtcCode("0xC03500");
    ASSERT_TRUE(hexPrefixed.has_value());
    EXPECT_EQ(hexPrefixed->code, "U0035");

    // Rejects invalid strings
    EXPECT_FALSE(parseDtcCode("").has_value());
    EXPECT_FALSE(parseDtcCode("X0300").has_value());
    EXPECT_FALSE(parseDtcCode("P4300").has_value());
    EXPECT_FALSE(parseDtcCode("P030").has_value());
    EXPECT_FALSE(parseDtcCode("P03000").has_value());
}

TEST(DtcCatalogTests, DecodesUdsStatusMaskIntoHumanReadableFlags)
{
    EXPECT_EQ(describeDtcStatus(0x00U), "cleared");
    EXPECT_EQ(describeDtcStatus(0x01U), "active");
    EXPECT_EQ(describeDtcStatus(0x08U), "confirmed");
    EXPECT_EQ(describeDtcStatus(0x2FU), "active, pending, confirmed");
    EXPECT_EQ(describeDtcStatus(0x89U), "active, confirmed, MIL on");
}

TEST(DtcCatalogTests, CatalogContainsAllCoreCodesWithUniqueValidEntries)
{
    const auto entries = dtcCatalogEntries();
    EXPECT_GE(entries.size(), 40U);

    std::set<std::string> seen;
    for (const DtcCatalogEntry& entry : entries) {
        SCOPED_TRACE(std::string{entry.code});
        EXPECT_TRUE(seen.insert(std::string{entry.code}).second) << "Duplicate DTC code in catalog";

        const auto parsed = parseDtcCode(entry.code);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->code, entry.code);

        EXPECT_FALSE(entry.summary.empty());
        EXPECT_FALSE(entry.j1939Equivalent.empty());
        EXPECT_FALSE(entry.firstChecks.empty());
    }

    // Verify key workshop reference codes from the guide and TorqueBus's shipped Lua UDS ECU
    for (const std::string_view code : {"P0171",
                                        "P0174",
                                        "P0300",
                                        "P0301",
                                        "P0420",
                                        "P0128",
                                        "P0299",
                                        "P0101",
                                        "P0562",
                                        "P0563",
                                        "C0035",
                                        "B1000",
                                        "U0035",
                                        "U0100",
                                        "U0121"}) {
        const DtcCatalogEntry* found = findDtcCatalogEntry(code);
        ASSERT_NE(found, nullptr) << "Missing catalog entry for " << code;
        EXPECT_EQ(found->code, code);
    }

    // Lookup is case-insensitive and accepts UDS hex or extended format
    const DtcCatalogEntry* lower = findDtcCatalogEntry("p0300");
    ASSERT_NE(lower, nullptr);
    EXPECT_EQ(lower->code, "P0300");

    const DtcCatalogEntry* fromHex = findDtcCatalogEntry("01 28 00");
    ASSERT_NE(fromHex, nullptr);
    EXPECT_EQ(fromHex->code, "P0128");

    // 6 contiguous hex digits starting with 'C' or 'B' (e.g. "C03500" for U0035-00)
    // must be parsed as 3-byte UDS hex rather than rejected as a 5-character SAE code.
    const std::optional<DtcStructure> udsC0 = parseDtcCode("C03500");
    ASSERT_TRUE(udsC0.has_value());
    EXPECT_EQ(udsC0->code, "U0035");
    EXPECT_TRUE(udsC0->hasFailureType);
    EXPECT_EQ(udsC0->failureType, 0x00U);
}
