#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# TorqueBus Studio
# Copyright (C) TorqueBus contributors
#
# Turns CSV exported from the SAE J1939 Digital Annex into the name table
# TorqueBus reads.
#
# --- Why this is a script and not a data file in the repository -------------
#
# The Digital Annex is a licensed commercial product of SAE International.
# Shipping its contents inside a GPL repository would be redistributing it,
# whatever that repository says about its own licence. So TorqueBus ships this
# converter and no data, and somebody who holds a licence runs it against their
# own copy, on their own machine, and keeps the result there.
#
# --- What to feed it --------------------------------------------------------
#
# The Digital Annex is distributed as a spreadsheet. Export these tabs to CSV -
# any spreadsheet program will, with File > Save As - and point this at them:
#
#   --manufacturers   the "Manufacturer Codes" tab            (Table B10)
#   --functions       the global NAME functions tab           (Table B11)
#   --ig-functions    the industry-group specific one         (Table B12)
#   --industry        the "Industry Groups" tab               (Table B1)
#
# All four are optional; give it whichever you have. Then:
#
#   python tools/j1939-names.py --manufacturers mfr.csv --functions fn.csv \
#       --ig-functions igfn.csv --out data/j1939-names.csv
#
# and put the result in a `data` directory beside TorqueBusStudio.exe.
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


def main():
    parser = argparse.ArgumentParser(
        description="Convert SAE J1939 Digital Annex CSV exports into the "
        "name table TorqueBus reads."
    )
    parser.add_argument("--manufacturers", help="CSV of the Manufacturer Codes tab")
    parser.add_argument("--functions", help="CSV of the global NAME Functions tab")
    parser.add_argument("--ig-functions", help="CSV of the IG specific NAME Functions tab")
    parser.add_argument("--industry", help="CSV of the Industry Groups tab")
    parser.add_argument("--out", required=True, help="where to write the table")

    arguments = parser.parse_args()

    if not any(
        [arguments.manufacturers, arguments.functions, arguments.ig_functions,
         arguments.industry]
    ):
        die("nothing to convert - pass at least one of the four inputs")

    lines = [
        "# J1939 NAME tables for TorqueBus.",
        "#",
        "# Generated by tools/j1939-names.py from the SAE J1939 Digital Annex.",
        "# The Digital Annex is licensed by SAE International; this file contains",
        "# its data and is not redistributable. Keep it on the machine that has",
        "# the licence.",
        "",
    ]

    counts = []

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

    with open(arguments.out, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")

    for what, many in counts:
        print("  %-30s %d" % (what, many))

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
