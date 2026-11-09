#!/usr/bin/env python3
"""Parse one NVIDIA SIDL instruction dump into instructions.json.

Usage:
    python3 parse_sidl.py <path to one instructions.txt> -o <output dir>

Standard library only, Python 3.11+.
"""

import argparse
import json
import os
import sys
from collections import Counter

from sidl_lex import SourceFile, bits_from_slot_name
from sidl_decls import (
    IGNORABLE_REGIONS,
    parse_declaration_block,
    parse_funit,
    parse_tables,
    split_regions,
)
from sidl_class import (
    find_class_blocks,
    parse_encoding,
    parse_format,
    parse_opcodes,
    parse_properties,
)

SCHEMA_VERSION = "v2.6"


def parse_file(path):
    src = SourceFile(path)
    problems = []
    stats = Counter()

    regions = {}
    for name, first, last in split_regions(src):
        regions.setdefault(name, (first, last))

    # -- declaration region --------------------------------------------------
    # One REGISTERS region declares every named type. Register groups and
    # modifier enumerations share one syntax there, so the text supports no
    # split between them and none is invented.
    enum_tables = {}
    if "REGISTERS" in regions:
        first, last = regions["REGISTERS"]
        # `first` is the REGISTERS header line itself; declarations start after it.
        enum_tables = parse_declaration_block(src, first + 1, last, problems)
    else:
        problems.append("no REGISTERS region")

    tables = {}
    if "TABLES" in regions:
        first, last = regions["TABLES"]
        tables = parse_tables(
            src, first, last, problems,
            {n: d["values"] for n, d in enum_tables.items()})
    else:
        problems.append("no TABLES region")

    encoding_width, funit_masks = (128, {})
    if "FUNIT" in regions:
        first, last = regions["FUNIT"]
        encoding_width, funit_masks = parse_funit(src, first, last, problems)
    else:
        problems.append("no FUNIT region")

    # Name -> member values. Used to decide whether a FORMAT type is declared
    # and to resolve the `\`Type@VALUE` right-hand sides of the sm_50..sm_62
    # dialect.
    enum_lookup = {n: d["values"] for n, d in enum_tables.items()}
    declared_types = set(enum_lookup)

    # -- class blocks --------------------------------------------------------
    instructions = {}
    assigned_slots = []
    opcode_values = set()
    blocks = find_class_blocks(src)
    for block in blocks:
        stats["class_" + block.kind.lower()] += 1
        where = f"{block.kind} {block.name!r} at line {block.first_line}"

        raw_format = ""
        bindings = {}
        conditions_raw = ""
        properties = {}
        mnemonics = []
        opcode_value = None
        class_opcode_values = []
        opcode_is_decimal = False
        encoding_section = None

        for keyword, text, first, last, offset in block.sections:
            stats["section_" + keyword] += 1
            if keyword == "FORMAT":
                raw_format, bindings = parse_format(
                    text, problems, where, declared_types)
            elif keyword == "FORMAT_ALIAS":
                stats["format_alias"] += 1
            elif keyword in ("CONDITIONS", "CONDITION"):
                conditions_raw = text.strip("\n").strip()
            elif keyword == "PROPERTIES":
                parse_properties(text, problems, where, properties)
            elif keyword == "PREDICATES":
                # Consumed and counted; the output schema has no field for them.
                stats["predicate_statements"] += text.count(";")
            elif keyword == "OPCODES":
                mnemonics, class_opcode_values, opcode_is_decimal = parse_opcodes(
                    text, problems, where)
                opcode_value = class_opcode_values[0] if class_opcode_values else None
            elif keyword == "ENCODING":
                encoding_section = (text, offset)

        opcode_values.update(class_opcode_values)

        slot_values = {}
        if encoding_section is not None:
            slot_values, used = parse_encoding(
                encoding_section[0], encoding_section[1], src, problems, where,
                bindings, opcode_value, opcode_is_decimal, enum_lookup, stats)
            assigned_slots.extend(used)
        else:
            problems.append(f"{where}: no ENCODING section")

        # ALTERNATE blocks are emitted alongside primary ones, flagged. They
        # encode instruction words of their own, and how their encoding relates
        # to a primary's varies, so the flag is all that is recorded.
        if block.name in instructions:
            problems.append(f"{where}: duplicate class name")
        entry = {
            "mnemonic": mnemonics,
            "format": raw_format,
            "slot_values": slot_values,
            "conditions_raw": conditions_raw,
            "properties": properties,
        }
        if block.kind != "CLASS":
            entry["is_alternate"] = True
        instructions[block.name] = entry

    # -- bit slot catalog ----------------------------------------------------
    # Restricted to slots some ENCODING statement assigns; the `!name;` slots
    # are excluded.
    catalog = {}
    mask_mismatches = 0
    for slot in sorted(set(assigned_slots)):
        mask_bits = funit_masks.get(slot)
        decoded = bits_from_slot_name(slot)
        if mask_bits is None and decoded is None:
            problems.append(f"slot {slot!r} has neither a FUNIT mask nor a BITS_ name")
            continue
        if mask_bits is not None and decoded is not None and decoded[0] != mask_bits:
            mask_mismatches += 1
            problems.append(f"slot {slot!r}: name-derived bits differ from FUNIT mask")
        bits = mask_bits if mask_bits is not None else decoded[0]
        catalog[slot] = {"bit_positions": bits, "width": len(bits)}

    # -- line coverage -------------------------------------------------------
    # Finest labels first so that coarse region labels only fill what is left.
    for block in blocks:
        for keyword, text, first, last, offset in block.sections:
            src.claim(first, last, "class." + keyword)
        src.claim(block.first_line, block.last_line, "class.header")
    for name, (first, last) in regions.items():
        label = "ignored." + name if name in IGNORABLE_REGIONS else "parsed." + name
        src.claim(first, last, label)

    coverage = Counter(o for o in src.owner if o is not None)
    leftovers = src.unclaimed()
    for lineno, text in leftovers:
        problems.append(f"line {lineno}: unclassified {text.strip()[:90]!r}")

    document = {
        "meta": {
            "source": os.path.basename(path),
            "schema_version": SCHEMA_VERSION,
            "variant_count": sum(1 for i in instructions.values() if not i.get("is_alternate")),
            "alternate_count": sum(1 for i in instructions.values() if i.get("is_alternate")),
            "distinct_opcode_count": len(opcode_values),
            "enum_table_count": len(enum_tables),
            "bit_slot_catalog_count": len(catalog),
            "table_count": len(tables),
            "mask_mismatches": mask_mismatches,
        },
        "instructions": instructions,
        "bit_slot_catalog": catalog,
        "enum_tables": enum_tables,
        "tables": tables,
    }
    report = {
        "encoding_width": encoding_width,
        "stats": stats,
        "coverage": coverage,
        "total_lines": len(src.lines),
        "unclassified_lines": len(leftovers),
        "problems": problems,
    }
    return document, report


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("input", help="one sm_*_instructions.txt dump")
    ap.add_argument("-o", "--output-dir", default=".", help="directory for instructions.json")
    ap.add_argument("--max-problems", type=int, default=25,
                    help="how many unparsed items to print (0 prints all)")
    args = ap.parse_args(argv)

    document, report = parse_file(args.input)

    os.makedirs(args.output_dir, exist_ok=True)
    out_path = os.path.join(args.output_dir, "instructions.json")
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(document, fh, indent=1)
        fh.write("\n")

    meta = document["meta"]
    print(f"source                 {meta['source']}")
    print(f"encoding width         {report['encoding_width']} bits")
    print(f"classes emitted        {meta['variant_count']} primary"
          f" + {meta['alternate_count']} alternate")
    print(f"distinct opcodes       {meta['distinct_opcode_count']}")
    print(f"encoded items          {sum(len(i['slot_values']) for i in document['instructions'].values())}"
          f"  from {report['stats']['assignments']} assignments"
          f", {report['stats']['unused']} !unused, {report['stats']['remap']} REMAP")
    print(f"bit slot catalog       {meta['bit_slot_catalog_count']}"
          f"  (mask mismatches {meta['mask_mismatches']})")
    print(f"enum tables            {meta['enum_table_count']}")
    print(f"tables                 {meta['table_count']}")
    print(f"wrote                  {out_path}")

    print("\nline coverage:")
    for label, count in sorted(report["coverage"].items()):
        print(f"  {count:8d}  {label}")
    print(f"  {report['unclassified_lines']:8d}  UNCLASSIFIED"
          f"   (of {report['total_lines']} lines)")

    problems = report["problems"]
    print(f"\nunparsed / suspect items: {len(problems)}")
    limit = len(problems) if args.max_problems == 0 else args.max_problems
    for p in problems[:limit]:
        print(f"  {p}")
    if len(problems) > limit:
        print(f"  ... {len(problems) - limit} more (use --max-problems 0)")
    # The specification is written either way, because a reported item is a
    # defect in one statement rather than a reason to discard the file. The
    # exit status says whether anything was reported.
    return 1 if (problems or report["unclassified_lines"]) else 0


if __name__ == "__main__":
    sys.exit(main())
