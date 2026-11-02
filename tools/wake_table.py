#!/usr/bin/env python3
"""components/skyscope/wake.h: every aircraft type's wake category, as the FAA's list of type designators gives it.

    pdftotext -layout JO_7360.1G_Aircraft_Type_Designators_FINAL.pdf 7360.txt
    tools/wake_table.py 7360.txt > components/skyscope/wake.h

FAA Order JO 7360.1 (https://www.faa.gov/documentLibrary/media/Order/JO_7360.1G_Aircraft_Type_Designators_FINAL.pdf)
lists, in its Appendix A, each designator with its Consolidated Wake Turbulence (CWT) category, A to I, the letter a
controller's scope shows. Run this again when the FAA publishes a new edition; a few rows the PDF lays out oddly are
put right by hand below.
"""

import re
import sys

# Rows whose columns the PDF's text does not keep apart, read off the order itself.
BY_HAND = {"A338": "D", "A35K": "D"}

ROW = re.compile(r"^\s{0,6}([A-Z0-9]{2,4})\*?\s{2,}(\S+(?: \S+)?)\s{2,}(\S+)\s{2,}(\S+)\s{2,}(\S+)\s{2,}([A-I])(?:\s|$)")


def table(text):
    lines = text.split("\n")
    start = next(i for i, line in enumerate(lines) if "Appendix A. Aircraft Type Designator" in line)
    end = next(i for i, line in enumerate(lines) if i > start and "Appendix B. Manufacturer" in line)
    out = {}
    for line in lines[start:end]:
        m = ROW.match(line)
        if m:
            out.setdefault(m.group(1), m.group(6))
    out.update(BY_HAND)
    return out


def header(wake):
    rows = [f'{{"{code}", \'{cwt}\'}}' for code, cwt in sorted(wake.items())]
    lines = []
    line = " "
    for row in rows:
        if len(line) + len(row) + 2 > 120:
            lines.append(line.rstrip())
            line = " "
        line += " " + row + ","
    lines.append(line.rstrip())
    return f"""#pragma once
// Made by tools/wake_table.py from FAA Order JO 7360.1G, Appendix A; do not edit by hand.
// Every aircraft type's Consolidated Wake Turbulence (CWT) category, A (super) to I (lower small), by its designator,
// in the designators' order so a type is found by halves.
#include <algorithm>
#include <cstring>

namespace sky::wake {{
struct Type {{
  char code[5];
  char cwt;
}};
// clang-format off
inline constexpr Type TYPES[] = {{
{chr(10).join(lines)}
}};
// clang-format on

// A type's category, or 0 where the FAA's list does not have it.
inline char of_type(const char *type) {{
  const Type *end = TYPES + sizeof(TYPES) / sizeof(TYPES[0]);
  const auto before = [](const Type &a, const char *b) {{ return std::strcmp(a.code, b) < 0; }};
  const Type *t = std::lower_bound(TYPES, end, type, before);
  return t != end && std::strcmp(t->code, type) == 0 ? t->cwt : 0;
}}
}}  // namespace sky::wake
"""


if __name__ == "__main__":
    with open(sys.argv[1], encoding="utf-8", errors="replace") as f:
        sys.stdout.write(header(table(f.read())))
