"""Top-level region splitting and the declaration regions of a SIDL dump.

Region boundaries, REGISTERS, the flattened enum
block, TABLES, FUNIT bit-slot masks, and the metadata-only regions.
"""

import re

from sidl_lex import (
    CLASS_HEADER_RE,
    IDENT,
    SIMPLE_REGION_KEYWORDS,
    bits_from_mask,
    parse_int_literal,
)

# Regions whose content the output schema does not carry. They are recognised and
# labelled so that "ignored" is visible rather than silent.
IGNORABLE_REGIONS = (
    "ARCHITECTURE",
    "PARAMETERS",
    "CONSTANTS",
    "STRING_MAP",
    "OPERATION PROPERTIES",
    "OPERATION PREDICATES",
    "NOP_ENCODING",
)


def split_regions(src):
    """Return [(region name, first line, last line)] over the whole file.

    Region keywords sit in column 0. The class region begins at the
    first class header.
    """
    marks = []
    for i, line in enumerate(src.lines):
        s = line.strip()
        if not s:
            continue
        at_col0 = line[:1] not in (" ", "\t")
        word = s.split()[0]
        if i == 0 and word == "ARCHITECTURE":
            marks.append((i, "ARCHITECTURE"))
        elif at_col0 and word in SIMPLE_REGION_KEYWORDS:
            marks.append((i, word))
        elif at_col0 and s == "TABLES":
            marks.append((i, "TABLES"))
        elif at_col0 and word == "OPERATION" and len(s.split()) > 1:
            marks.append((i, "OPERATION " + s.split()[1].rstrip(";")))
        elif at_col0 and word == "FUNIT":
            marks.append((i, "FUNIT"))

    m = CLASS_HEADER_RE.search(src.text)
    if m:
        marks.append((src.text[:m.start()].count("\n"), "CLASSES"))
    marks.sort()

    regions = []
    for k, (i, name) in enumerate(marks):
        end = marks[k + 1][0] if k + 1 < len(marks) else len(src.lines)
        regions.append((name, i + 1, end))
    return regions


# ---------------------------------------------------------------------------
# REGISTERS and the flattened enum block
# ---------------------------------------------------------------------------

def _entries(src, first, last):
    """Yield (line number, text) for `;`-terminated declaration entries.

    Entries wrap across lines, so the region is reflowed and cut on `;`.
    """
    buf = ""
    start = None
    for i in range(first - 1, last):
        s = src.lines[i].strip()
        if not s:
            continue
        if start is None:
            start = i + 1
        buf = (buf + " " + s) if buf else s
        while ";" in buf:
            cut = buf.index(";")
            entry = buf[:cut].strip()
            if entry:
                yield start, entry
            buf = buf[cut + 1:].strip()
            start = i + 1
    if buf.strip():
        yield start, buf.strip()


# One member of a declaration. The item forms that occur are:
#   NAME | "NAME" | NAME=<int> | "NAME"=<int> | NAME(lo..hi) | NAME(lo..hi)=(lo..hi)
#   | NAME* | NAME *= <int>
_MEMBER_RE = re.compile(
    r'^(?P<name>"[^"]*"|[A-Za-z_][A-Za-z_0-9.]*)'
    r'(?:\((?P<rlo>\d+)\.\.(?P<rhi>\d+)\))?'
    r'\s*(?P<star>\*)?'
    r'(?:=\s*(?:\((?P<vlo>\d+)\.\.(?P<vhi>\d+)\)|(?P<val>0[bB][01_]+|0[xX][0-9a-fA-F_]+|\d+)))?$'
)

_UNION_RE = re.compile(rf"^(?P<name>{IDENT})\s*=\s*(?P<parts>{IDENT}(?:\s*\+\s*{IDENT})*)$")


def parse_declaration(entry, groups, problems, where):
    """Parse one REGISTERS / enum-block entry into (name, values, default).

    `groups` is the already-parsed mapping, needed to resolve union entries.
    """
    union = _UNION_RE.match(entry)
    if union:
        # Union entry: `RegisterFAU = NonZeroRegisterFAU + ZeroRegister;`
        merged = {}
        for part in re.split(r"\s*\+\s*", union.group("parts")):
            src_group = groups.get(part)
            if src_group is None:
                problems.append(f"{where}: union references undeclared group {part!r}")
                continue
            merged.update(src_group["values"])
        return union.group("name"), merged, None

    head = entry.split(None, 1)
    name = head[0]
    body = head[1] if len(head) > 1 else ""

    values = {}
    default = None
    next_value = 0
    for raw in body.split(","):
        item = raw.strip()
        if not item:
            continue
        m = _MEMBER_RE.match(item)
        if not m:
            problems.append(f"{where}: unparsed declaration member {item!r} in {name!r}")
            continue
        member = m.group("name")
        if member.startswith('"'):
            member = member[1:-1]

        if m.group("rlo") is not None:
            # Generated names: `SB(0..5)` or `R(0..254)=(0..254)`.
            lo, hi = int(m.group("rlo")), int(m.group("rhi"))
            if m.group("vlo") is not None:
                vlo = int(m.group("vlo"))
            else:
                vlo = next_value
            for k, suffix in enumerate(range(lo, hi + 1)):
                values[f"{member}{suffix}"] = vlo + k
            next_value = vlo + (hi - lo) + 1
            continue

        if m.group("val") is not None:
            parsed = parse_int_literal(m.group("val"))
            value = parsed[0]
        else:
            # Implicit numbering: previous value + 1, starting at 0.
            value = next_value
        values[member] = value
        next_value = value + 1

        if m.group("star") and m.group("val") is not None:
            # `*=` marks the type default. Exactly one per file.
            default = member
    return name, values, default


def parse_declaration_block(src, first, last, problems):
    """Parse the declarations of a region into {name: {values, default}}."""
    out = {}
    for lineno, entry in _entries(src, first, last):
        where = f"line {lineno}"
        name, values, default = parse_declaration(entry, out, problems, where)
        if name in out:
            problems.append(f"{where}: duplicate declaration {name!r}")
        out[name] = {"values": values, "default": default}
    return out


# ---------------------------------------------------------------------------
# TABLES
# ---------------------------------------------------------------------------

_TABLE_NAME_RE = re.compile(rf"^{IDENT}$")

_ENUMREF_TOKEN_RE = re.compile(rf'^`?(?P<type>{IDENT})@(?P<member>"[^"]*"|[A-Za-z_0-9.]+)$')


def _resolve_token(tok, declared_types):
    """Return a row token's integer value, or the token itself when it has none.

    Integer literals resolve by spelling; `Type@MEMBER` references resolve
    through the declaration of that type.
    """
    parsed = parse_int_literal(tok)
    if parsed is not None:
        return parsed[0]
    m = _ENUMREF_TOKEN_RE.match(tok)
    if m and declared_types:
        member = m.group("member").strip('"')
        values = declared_types.get(m.group("type"))
        if values is not None and member in values:
            return values[member]
    return tok


def _split_rows(lines, problems):
    """Turn a table's raw `<inputs> -> <output>` lines into rows.

    Four lines across the five architectures carry two rows each. Some separate
    them with a run of spaces; the rest run the first row's output together with
    the next row's first input, so `1 1 0 -> 332 1 0 -> 65` is `1 1 0 -> 33`
    followed by `2 1 0 -> 65`. The table's own columns decide where such a token divides:
    the segment between two arrows must hold one output followed by a whole row
    of inputs, and the input it yields must be a value that column takes in the
    rows that are not in doubt.
    """
    simple, pending = [], []
    for lineno, text in lines:
        parts = text.split("->")
        if len(parts) == 2:
            simple.append((parts[0].split(), parts[1].strip()))
        else:
            pending.append((lineno, parts))
    if not pending:
        return simple

    width = max((len(r[0]) for r in simple), default=0)
    columns = [{r[0][i] for r in simple if len(r[0]) > i} for i in range(width)]

    rows = list(simple)
    for lineno, parts in pending:
        resolved, inputs, ok = [], parts[0].split(), True
        for seg in parts[1:-1]:
            tokens = seg.split()
            if len(tokens) == width + 1:
                cuts = [(tokens[0], tokens[1:])]
            else:
                first = tokens[0] if tokens else ""
                cuts = [(first[:k], [first[k:]] + tokens[1:])
                        for k in range(1, len(first))
                        if len(tokens) + 1 == width + 1
                        and first[k:] in columns[0]]
            if len(cuts) != 1:
                problems.append(
                    f"line {lineno}: cannot divide {seg.strip()!r} into an "
                    f"output and {width} inputs")
                ok = False
                break
            value, next_inputs = cuts[0]
            resolved.append((inputs, value))
            inputs = next_inputs
        if ok:
            resolved.append((inputs, parts[-1].strip()))
            rows.extend(resolved)
        else:
            rows.append((parts[0].split(), "->".join(parts[1:]).strip()))
    return rows


def parse_tables(src, first, last, problems, declared_types=None):
    """Parse the TABLES region into {name: {n_inputs, n_rows, encode, decode}}.

    Row tokens come in three spellings: integer literals in any of
    the three bases, including binary outputs such as `CNSConstBank`'s `0b01`;
    enum references such as `ARegAlignment`'s `AInteger@"32"`; and bare words.
    Each token is resolved to its integer value where the spelling or the
    declared type gives one, and otherwise kept verbatim. A row whose inputs
    duplicate an earlier row is dropped, silently when its output matches and
    with a report when it differs, so `n_rows`, which counts the rows as
    written, can exceed the number of `encode` keys.
    """
    tables = {}
    current = None
    raw_lines = []

    def flush():
        nonlocal current, raw_lines
        if current is None:
            return
        rows = _split_rows(raw_lines, problems)
        n_inputs = max((len(r[0]) for r in rows), default=0)
        encode, decode = {}, {}
        for ins, out in rows:
            key = ",".join(str(_resolve_token(t, declared_types)) for t in ins)
            value = _resolve_token(out, declared_types)
            if key in encode:
                # The description repeats an argument tuple in two ways. With
                # the same output the row is redundant; with a different one
                # (sm_70's `TABLES_opex_1` maps `0 17 0 1` to both 145 and 81)
                # keeping the last would leave `encode` and `decode`
                # disagreeing. The first wins either way.
                if encode[key] != value:
                    problems.append(
                        f"table {current!r}: inputs {key} map to both "
                        f"{encode[key]} and {value}; keeping the first")
                continue
            encode[key] = value
            decode.setdefault(str(value), []).append(key)
        tables[current] = {
            "n_inputs": n_inputs,
            "n_rows": len(rows),
            "encode": encode,
            "decode": decode,
        }
        current, raw_lines = None, []

    for i in range(first, last):          # first is the `TABLES` header line
        s = src.lines[i].strip()
        if not s:
            continue
        if s == ";":                      # lone terminator ends a table
            flush()
            continue
        if "->" in s:
            raw_lines.append((i + 1, s))
        elif _TABLE_NAME_RE.match(s):
            flush()
            current = s
        else:
            problems.append(f"line {i + 1}: unparsed TABLES line {s!r}")
    flush()
    return tables


# ---------------------------------------------------------------------------
# FUNIT bit-slot masks
# ---------------------------------------------------------------------------

_MASK_RE = re.compile(r"([A-Za-z_][A-Za-z_0-9.]*)\s+'([.X]+)'")
_WIDTH_RE = re.compile(r"^ENCODING\s+WIDTH\s+(\d+)\s*;")


def parse_funit(src, first, last, problems):
    """Return (encoding width, {slot name: bit positions}).

    Two or more `name '<mask>'` pairs may share one line, so the region is scanned
    by regular expression rather than one entry per line.
    """
    width = None
    for i in range(first, last):
        m = _WIDTH_RE.match(src.lines[i].strip())
        if m:
            width = int(m.group(1))
            break
    if width is None:
        problems.append("FUNIT: no ENCODING WIDTH found")
        width = 128

    masks = {}
    for i in range(first, last):
        for m in _MASK_RE.finditer(src.lines[i]):
            name, mask = m.group(1), m.group(2)
            if len(mask) != width:
                problems.append(
                    f"line {i + 1}: slot {name!r} mask is {len(mask)} chars, width is {width}")
                continue
            masks[name] = bits_from_mask(mask, width)
    return width, masks
