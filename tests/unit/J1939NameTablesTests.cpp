// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Turning the numbers in a NAME into words.
//
// The case that carries the file is the one about function numbers at or above
// 128: the same number is a different device on a tractor and on a boat, and a
// lookup that ignored the industry group would answer confidently and wrongly
// for half the range. That is the reason this data is a file at all.
//
// Every name here is invented. The real tables are the SAE Digital Annex, which
// is licensed and is not ours to ship - so the tests exercise the mechanism
// with data that could not have come from it.

#include "core/j1939/J1939NameTables.h"

#include <gtest/gtest.h>

#include <string>

using namespace torquebus;

namespace {

[[nodiscard]] J1939Name
nameWith(std::uint8_t function, std::uint8_t industryGroup = 0U, std::uint8_t vehicleSystem = 0U)
{
    J1939Name name;
    name.function = function;
    name.industryGroup = industryGroup;
    name.vehicleSystem = vehicleSystem;

    return name;
}

} // namespace

TEST(J1939NameTablesTests, AFunctionBelow128IsNamedByItsNumberAlone)
{
    J1939NameTables tables;
    ASSERT_TRUE(tables.load("function,3,Gearbox Of Some Kind\n").succeeded());

    // Whatever machine it is on: the low range is industry group independent,
    // so the answer must not change with the rest of the NAME.
    EXPECT_TRUE(tables.function(nameWith(3U)) == "Gearbox Of Some Kind");
    EXPECT_TRUE(tables.function(nameWith(3U, 2U, 5U)) == "Gearbox Of Some Kind");
    EXPECT_TRUE(tables.function(nameWith(4U)) == std::nullopt);
}

TEST(J1939NameTablesTests, AFunctionAtOrAbove128IsADifferentDeviceOnADifferentMachine)
{
    // The whole reason these tables are a file. Function 130 on an
    // agricultural machine and function 130 on a boat are two devices, and a
    // lookup by number alone would name one of them wrongly and never say so.
    J1939NameTables tables;
    ASSERT_TRUE(tables
                    .load("function,2/1/130,Something Agricultural\n"
                          "function,4/1/130,Something Marine\n")
                    .succeeded());

    EXPECT_TRUE(tables.function(nameWith(130U, 2U, 1U)) == "Something Agricultural");
    EXPECT_TRUE(tables.function(nameWith(130U, 4U, 1U)) == "Something Marine");

    // And a combination the file does not cover gets no name rather than the
    // nearest one.
    EXPECT_TRUE(tables.function(nameWith(130U, 3U, 1U)) == std::nullopt);
    EXPECT_TRUE(tables.function(nameWith(130U, 2U, 9U)) == std::nullopt);
}

TEST(J1939NameTablesTests, AHighFunctionNumberWrittenAsABareNumberIsRefused)
{
    // It would load, and then answer the same for every industry group - which
    // is the failure this format exists to prevent. Better to refuse the line
    // and name it.
    J1939NameTables tables;
    const Result result = tables.load("function,200,Ambiguous\n");

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(result.message().find("128") != std::string::npos);
}

TEST(J1939NameTablesTests, ManufacturersAndIndustryGroupsAreRead)
{
    J1939NameTables tables;
    ASSERT_TRUE(tables
                    .load("# a comment, ignored\n"
                          "\n"
                          "manufacturer,33,Acme, Inc.\n"
                          "industry,2,Agricultural And Forestry\n")
                    .succeeded());

    // Everything after the second comma is the name, commas included, so a
    // company with one in its name needs no escaping and nobody has to
    // remember a rule.
    EXPECT_TRUE(tables.manufacturer(33U) == "Acme, Inc.");
    EXPECT_TRUE(tables.industryGroup(2U) == "Agricultural And Forestry");

    EXPECT_TRUE(tables.manufacturer(34U) == std::nullopt);
    EXPECT_TRUE(tables.manufacturerCount() == 1U);
    EXPECT_TRUE(tables.industryGroupCount() == 1U);
}

TEST(J1939NameTablesTests, AMalformedLineIsRefusedWithItsNumberNotSkipped)
{
    // A table that quietly drops half its rows answers some questions and not
    // others, and nobody can tell which - so a bad line stops the load and says
    // where it is.
    J1939NameTables tables;

    const Result missingField = tables.load("manufacturer,33\n");
    ASSERT_TRUE(missingField.failed());
    EXPECT_TRUE(missingField.message().find("Line 1") != std::string::npos);

    const Result badKind = tables.load("company,33,Acme\n");
    ASSERT_TRUE(badKind.failed());
    EXPECT_TRUE(badKind.message().find("industry, manufacturer or function") != std::string::npos);

    const Result outOfRange = tables.load("industry,9,Too Big\n");
    ASSERT_TRUE(outOfRange.failed());
    EXPECT_TRUE(outOfRange.message().find("0 to 7") != std::string::npos);

    // A key that was mistyped must not silently name a different thing.
    const Result trailing = tables.load("manufacturer,33x,Acme\n");
    ASSERT_TRUE(trailing.failed());

    // And a refused load leaves nothing half-read behind.
    EXPECT_TRUE(tables.empty());
}

TEST(J1939NameTablesTests, NoFileAtAllIsNotAFailure)
{
    // What a machine without a licensed copy of the Digital Annex looks like.
    // Everything still works; the panel shows numbers where the words would be.
    J1939NameTables tables;

    EXPECT_TRUE(tables.loadFile("there-is-no-such-file.csv").succeeded());
    EXPECT_TRUE(tables.empty());
    EXPECT_TRUE(tables.function(nameWith(3U)) == std::nullopt);
}
