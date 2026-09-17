#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# TorqueBus Studio
# Copyright (C) TorqueBus contributors
#
# Builds the J1939 NAME tables TorqueBus reads.
#
# --- Which half ships, and why ----------------------------------------------
#
# **Function names ship with TorqueBus.** They are generated from AgIsoStack++,
# which is MIT licensed and therefore ours to pass on with its notice attached.
# `data/j1939-names-functions.csv` in this repository is the output of
#
#   python tools/j1939-names.py --agisostack can_NAME.hpp \
#       --out data/j1939-names-functions.csv
#
# and is regenerated the same way when AgIsoStack moves.
#
# **Manufacturer names do not.** That registry is the SAE J1939 Digital Annex,
# a licensed commercial product; the public copy at isobus.net states no licence
# either. Shipping it would be redistributing somebody else's database on an
# assumption - so this builds that half on the machine of whoever wants it.
#
# --- Building the manufacturer table ----------------------------------------
#
# From a licensed Digital Annex, which is distributed as a spreadsheet. Export
# the tabs to CSV - any spreadsheet program will, with File > Save As:
#
#   --manufacturers   the "Manufacturer Codes" tab            (Table B10)
#   --functions       the global NAME functions tab           (Table B11)
#   --ig-functions    the industry-group specific one         (Table B12)
#   --industry        the "Industry Groups" tab               (Table B1)
#
# Or from the public ISO 11783 registry, which needs nothing but a connection:
#
#   --isobus-net
#
# Then put the result beside TorqueBusStudio.exe as `data/j1939-names.csv`.
# TorqueBus loads the shipped functions first and yours on top, so a full
# Digital Annex table corrects the partial one rather than sitting beside it:
#
#   python tools/j1939-names.py --isobus-net --out data/j1939-names.csv
#
# --- It refuses rather than guesses -----------------------------------------
#
# Column headings in the Annex have changed between editions and will change
# again. This finds them by name, and when it cannot find one it says which
# file and which heading and stops. A converter that guessed a column would
# produce a table that is wrong in a way nobody would ever check.

import argparse
import csv
import sys


def die(message):
    print("error: " + message, file=sys.stderr)
    sys.exit(1)


def find_column(headers, wanted, path):
    """The index of the first heading containing every word in `wanted`."""
    lowered = [h.strip().lower() for h in headers]

    for index, heading in enumerate(lowered):
        if all(word in heading for word in wanted):
            return index

    die(
        "%s: no column heading contains %s.\n"
        "       headings found: %s\n"
        "       The Digital Annex has renamed columns between editions; pass the\n"
        "       right tab, or adjust the wanted words at the top of this script."
        % (path, " and ".join(repr(w) for w in wanted), ", ".join(headers))
    )


def read_rows(path):
    """Header row plus data rows, skipping anything before the real header."""
    with open(path, newline="", encoding="utf-8-sig", errors="replace") as handle:
        rows = [row for row in csv.reader(handle) if any(cell.strip() for cell in row)]

    if not rows:
        die("%s: empty" % path)

    return rows[0], rows[1:]


def whole_number(text):
    """The number, or None. Refuses anything that is not entirely a number."""
    text = text.strip()
    if not text:
        return None

    try:
        return int(text)
    except ValueError:
        return None


def convert_manufacturers(path, out):
    headers, rows = read_rows(path)
    code_at = find_column(headers, ["manufacturer", "code"], path)
    name_at = find_column(headers, ["manufacturer", "name"], path)

    written = 0
    for row in rows:
        if max(code_at, name_at) >= len(row):
            continue

        code = whole_number(row[code_at])
        name = row[name_at].strip()

        if code is None or not name or not 0 <= code <= 2047:
            continue

        out.append("manufacturer,%d,%s" % (code, name))
        written += 1

    return written


def convert_functions(path, out):
    headers, rows = read_rows(path)
    id_at = find_column(headers, ["function", "id"], path)
    name_at = find_column(headers, ["function", "name"], path)

    written = 0
    for row in rows:
        if max(id_at, name_at) >= len(row):
            continue

        function = whole_number(row[id_at])
        name = row[name_at].strip()

        if function is None or not name:
            continue

        # The global tab is the industry-group-independent range. Anything at or
        # above 128 in it is a row this script does not understand, and writing
        # it as a bare number would make it answer the same for every machine.
        if not 0 <= function < 128:
            continue

        out.append("function,%d,%s" % (function, name))
        written += 1

    return written


def convert_ig_functions(path, out):
    headers, rows = read_rows(path)
    group_at = find_column(headers, ["industry", "group", "id"], path)
    system_at = find_column(headers, ["vehicle", "system", "id"], path)
    id_at = find_column(headers, ["function", "id"], path)
    name_at = find_column(headers, ["function", "name"], path)

    written = 0
    for row in rows:
        if max(group_at, system_at, id_at, name_at) >= len(row):
            continue

        group = whole_number(row[group_at])
        system = whole_number(row[system_at])
        function = whole_number(row[id_at])
        name = row[name_at].strip()

        if None in (group, system, function) or not name:
            continue

        if not (0 <= group <= 7 and 0 <= system <= 127 and 0 <= function <= 255):
            continue

        out.append("function,%d/%d/%d,%s" % (group, system, function, name))
        written += 1

    return written


def convert_industry(path, out):
    headers, rows = read_rows(path)
    id_at = find_column(headers, ["industry", "group", "id"], path)
    name_at = find_column(headers, ["industry", "group"], path)

    # When one heading matched both, the name is whichever other column is not
    # the id - the Annex has spelled this pair several ways.
    if name_at == id_at:
        name_at = find_column(headers, ["description"], path)

    written = 0
    for row in rows:
        if max(id_at, name_at) >= len(row):
            continue

        group = whole_number(row[id_at])
        name = row[name_at].strip()

        if group is None or not name or not 0 <= group <= 7:
            continue

        out.append("industry,%d,%s" % (group, name))
        written += 1

    return written


def convert_agisostack(path, out):
    """The Function enum from AgIsoStack++ (MIT), with its section comments.

    The comments carry what the enum cannot: a function number at or above 128
    means different things in different industry groups, and the enum has the
    same number several times over. So the block headings are the key, and a
    block whose heading does not give both an industry group and a device class
    is **refused** rather than guessed at - the same rule the file format
    itself enforces.
    """
    industry_group = None
    device_class = None
    in_common = False
    in_enum = False

    written = 0
    skipped = []

    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            stripped = line.strip()

            if "enum class Function" in stripped:
                in_enum = True
                continue

            if not in_enum:
                continue

            if stripped.startswith("};"):
                break

            if stripped.startswith("//"):
                comment = stripped.lstrip("/ ").strip()
                lowered = comment.lower()

                if "common function" in lowered:
                    in_common, industry_group, device_class = True, None, None
                    continue

                in_common = False

                # A banner - "******** Marine (Industry Group 4) ********" -
                # sets the industry group for everything under it. The
                # sub-block headings below it usually name only a device class,
                # so the group has to be sticky or every one of them is lost.
                named_group = _number_after(lowered, "industry group")
                if named_group is not None:
                    industry_group = named_group

                # The device class is not sticky. A heading that names none -
                # "Propulsion Systems" - is a block this script cannot place,
                # and carrying the previous class into it would file its
                # functions under the wrong device entirely.
                device_class = _number_after(lowered, "device class")
                continue

            if "=" not in stripped:
                continue

            name, _, rest = stripped.partition("=")
            value = whole_number(rest.split(",")[0])
            if value is None:
                continue

            label = _spaced(name.strip())

            if in_common:
                if value < 128:
                    out.append("function,%d,%s" % (value, label))
                    written += 1
                else:
                    # In the industry-group-independent block and yet above the
                    # line where the number stops being independent. Something
                    # is off in the source, and a bare number here would answer
                    # the same for every machine.
                    skipped.append("%s = %d (in the common block, but >= 128)" % (label, value))
                continue

            if industry_group is None or device_class is None:
                skipped.append("%s = %d (its block names no industry group and device class)"
                               % (label, value))
                continue

            out.append("function,%d/%d/%d,%s" % (industry_group, device_class, value, label))
            written += 1

    written -= _refuse_duplicate_keys(out, skipped)

    return written, skipped


def _refuse_duplicate_keys(lines, skipped):
    """Removes every entry whose key another entry also claims. Returns how many.

    A key that two lines answer differently answers neither reliably, and the
    loader takes the last one - so shipping both means shipping a wrong answer
    with no sign that it is wrong.

    This is not hypothetical. AgIsoStack's On-Highway section has two blocks
    whose headings are both "Non-specific system (Device class 0) industry
    group 1" - the second adds the word "Tractor" in prose and nothing else.
    On-Highway vehicle system 1 *is* Tractor, so the second block is almost
    certainly vehicle system 1 and the comment is sloppy. Almost certainly is
    not good enough: this script refuses a block that names no device class
    rather than carrying the previous one over, and inferring a class from an
    adjective would be the same guess wearing a better disguise.

    So both go, and both are listed. Somebody with a Digital Annex gets the
    right answer from the right source, and their table overrides this one.
    """
    import collections

    seen = collections.defaultdict(list)

    for index, line in enumerate(lines):
        if not line.startswith("function,"):
            continue
        _, key, _ = line.split(",", 2)
        seen[key].append(index)

    doomed = set()

    for key, positions in sorted(seen.items()):
        if len(positions) < 2:
            continue

        names = [lines[i].split(",", 2)[2] for i in positions]
        skipped.append("function %s (claimed by %d entries: %s)"
                       % (key, len(positions), ", ".join(names)))
        doomed.update(positions)

    if not doomed:
        return 0

    for index in sorted(doomed, reverse=True):
        del lines[index]

    return len(doomed)


def _number_after(text, phrase):
    """The first number following `phrase`, or None."""
    where = text.find(phrase)
    if where < 0:
        return None

    digits = ""
    for character in text[where + len(phrase):]:
        if character.isdigit():
            digits += character
        elif digits:
            break

    return int(digits) if digits else None


def _spaced(identifier):
    """TaskController -> Task Controller. Left alone if it is already words."""
    out = []
    for index, character in enumerate(identifier):
        if index and character.isupper() and not identifier[index - 1].isupper():
            out.append(" ")
        out.append(character)

    return "".join(out)


def fetch_isobus_net(out):
    """The manufacturer registry published at isobus.net.

    That page is the official ISO 11783 registry, maintained by VDMA, and it is
    public - which is what makes resolving a code possible at all. It states no
    licence, so this fetches it onto the machine that asked and nothing more.
    Whether the result may be passed on to anybody else is a question for
    whoever reads their terms, and this script does not answer it.

    One request per page, paced, because a registry is a courtesy and hammering
    it is how courtesies end.
    """
    import time
    import urllib.request

    base = "https://www.isobus.net/isobus/manufacturerCode"
    written = 0
    page = 0

    print("  fetching from %s" % base)

    while True:
        url = base if page == 0 else "%s?page=%d" % (base, page)

        try:
            request = urllib.request.Request(
                url, headers={"User-Agent": "TorqueBus j1939-names.py"}
            )
            with urllib.request.urlopen(request, timeout=30) as response:
                html = response.read().decode("utf-8", errors="replace")
        except Exception as problem:
            if page == 0:
                die("could not reach %s: %s" % (url, problem))
            break

        found = _rows_from_html(html)
        if not found:
            break

        for code, name in found:
            out.append("manufacturer,%d,%s" % (code, name))
            written += 1

        page += 1
        if page > 40:
            # A registry of this size is about 17 pages. Forty is a stop that
            # exists so a change in the page shape cannot turn this into a loop
            # that keeps asking somebody else server for ever.
            print("  stopping at 40 pages", file=sys.stderr)
            break

        time.sleep(1.0)

    return written


def _rows_from_html(html):
    """(code, name) pairs from a registry page, or an empty list."""
    import html as html_module
    import re

    rows = []

    for cells in re.findall(r"<tr[^>]*>(.*?)</tr>", html, re.S | re.I):
        values = [
            html_module.unescape(re.sub(r"<[^>]+>", " ", cell)).strip()
            for cell in re.findall(r"<td[^>]*>(.*?)</td>", cells, re.S | re.I)
        ]

        if len(values) < 2:
            continue

        code = whole_number(values[0])
        name = " ".join(values[1].split())

        if code is None or not name or not 0 <= code <= 2047:
            continue

        rows.append((code, name))

    return rows


def main():
    parser = argparse.ArgumentParser(
        description="Convert SAE J1939 Digital Annex CSV exports into the "
        "name table TorqueBus reads."
    )
    parser.add_argument("--manufacturers", help="CSV of the Manufacturer Codes tab")
    parser.add_argument("--functions", help="CSV of the global NAME Functions tab")
    parser.add_argument("--ig-functions", help="CSV of the IG specific NAME Functions tab")
    parser.add_argument("--industry", help="CSV of the Industry Groups tab")
    parser.add_argument(
        "--agisostack",
        help="can_NAME.hpp from AgIsoStack++ (MIT), as the source of function names",
    )
    parser.add_argument(
        "--isobus-net",
        action="store_true",
        help="fetch manufacturer codes from the public registry at isobus.net",
    )
    parser.add_argument("--out", required=True, help="where to write the table")

    arguments = parser.parse_args()

    if not any(
        [arguments.manufacturers, arguments.functions, arguments.ig_functions,
         arguments.industry, arguments.agisostack, arguments.isobus_net]
    ):
        die("nothing to convert - pass at least one input")

    from_annex = any([arguments.manufacturers, arguments.functions,
                      arguments.ig_functions, arguments.industry])

    lines = ["# J1939 NAME tables for TorqueBus.", "#",
             "# Generated by tools/j1939-names.py. Do not edit by hand; edit the",
             "# sources and run it again.", "#"]

    if arguments.agisostack:
        lines += [
            "# Function names derived from AgIsoStack++ by Open-Agriculture,",
            "# which is MIT licensed:",
            "#",
            "#   Copyright (c) Open-Agriculture contributors",
            "#   https://github.com/Open-Agriculture/AgIsoStack-plus-plus",
            "#",
            "# Permission is hereby granted, free of charge, to any person obtaining",
            "# a copy of this software and associated documentation files, to deal",
            "# in the Software without restriction. The MIT licence text travels",
            "# with this notice; see the project above for the full terms.",
            "#",
        ]

    if arguments.isobus_net:
        lines += [
            "# Manufacturer names fetched from the public ISO 11783 registry at",
            "# isobus.net, maintained by VDMA. That page states no licence. This",
            "# file was built on this machine for use on it; passing it on is a",
            "# question for whoever reads their terms.",
            "#",
        ]

    if from_annex:
        lines += [
            "# Some of this came from the SAE J1939 Digital Annex, which is licensed",
            "# by SAE International. A file containing its data is NOT",
            "# redistributable - keep it on the machine that holds the licence.",
            "#",
        ]

    lines.append("")

    counts = []
    refused = []

    if arguments.agisostack:
        many, skipped = convert_agisostack(arguments.agisostack, lines)
        counts.append(("functions from AgIsoStack", many))
        refused.extend(skipped)

    if arguments.industry:
        counts.append(("industry groups", convert_industry(arguments.industry, lines)))
    if arguments.functions:
        counts.append(("global functions", convert_functions(arguments.functions, lines)))
    if arguments.ig_functions:
        counts.append(
            ("industry specific functions", convert_ig_functions(arguments.ig_functions, lines))
        )
    if arguments.manufacturers:
        counts.append(
            ("manufacturers", convert_manufacturers(arguments.manufacturers, lines))
        )
    if arguments.isobus_net:
        counts.append(("manufacturers from isobus.net", fetch_isobus_net(lines)))

    if refused:
        # Written into the file, not only printed. Somebody opening it in a year
        # should be able to see that it is partial without re-running anything,
        # and know that a fuller one is a Digital Annex licence away.
        note = [
            "# This table is NOT complete. %d entries were left out, for two"
            % len(refused),
            "# reasons, and neither one is safe to guess past:",
            "#",
            "#   * Their block did not say which industry group and vehicle",
            "#     system they belong to, and a function number at or above 128",
            "#     means different things in different ones.",
            "#",
            "#   * Two entries claimed the same key with different names. One of",
            "#     them is right and the source does not say which, so neither",
            "#     ships: a key that answers two things answers neither.",
            "#",
            "# Run tools/j1939-names.py against a licensed SAE Digital Annex for a",
            "# full table; it will override this one.",
            "#",
        ]
        lines[5:5] = note

    with open(arguments.out, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")

    for what, many in counts:
        print("  %-30s %d" % (what, many))

    if refused:
        # Named rather than counted. Each of these is a function somebody will
        # look for and not find, and the reason is in the source rather than
        # here.
        print("\nrefused %d entries whose industry group could not be read:" % len(refused))
        for entry in refused:
            print("  %s" % entry)

    print("\nwrote %s" % arguments.out)

    if any(many == 0 for _, many in counts):
        # Said rather than left for somebody to notice later: a file that
        # converted zero rows looks exactly like one that worked, right up
        # until a panel shows numbers where names should be.
        print(
            "\nwarning: one of the inputs produced no rows. Check that the CSV is the\n"
            "         tab it is supposed to be.",
            file=sys.stderr,
        )


if __name__ == "__main__":
    main()
