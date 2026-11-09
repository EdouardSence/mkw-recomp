"""CLASS / ALTERNATE CLASS block parsing.

Class headers, sub-section splitting, FORMAT
bindings, OPCODES, CONDITIONS, PROPERTIES, PREDICATES and the ENCODING
assignment grammar that produces `slot_values`.
"""

import re

from sidl_lex import (
    CLASS_HEADER_RE,
    DOTTED,
    IDENT,
    MNEMONIC,
    SUBSECTION_KEYWORDS,
    bits_from_slot_name,
    parse_int_literal,
    scan_words,
    split_at_semicolon,
    split_statements,
)

# A FORMAT binding. The optional leading '/' marks a modifier
# binding; '*' and the '@' before ':' are flags that occur but carry no schema
# meaning.
BINDING_RE = re.compile(
    rf"(?P<mod>/)?(?P<type>{DOTTED})"
    rf"(?:\((?P<args>[^()]*)\))?"
    rf"(?P<star>\*)?(?P<at>@)?:(?P<name>{DOTTED})"
)

# The five `@` post-operators that occur in the corpus. No other suffix occurs.
# `@sign` is sm_50..sm_62 only, and there only inside a call argument list, so
# it never reaches the right-hand side this tuple is checked against.
POST_OPERATORS = ("not", "negate", "absolute", "invert", "sign")

# The three word-operators that follow a variable.
WORD_OPERATORS = ("SCALE", "MULTIPLY", "convertFloatType")

_CALL_RE = re.compile(rf"^(?P<name>{IDENT})\s*\((?P<args>.*)\)$", re.S)
_ENUMREF_RE = re.compile(rf"^`(?P<type>{IDENT})@(?P<value>\"[^\"]*\"|[A-Za-z_0-9.]+)$")
_FIELD_RE = re.compile(rf"^(?P<name>{DOTTED})(?:@(?P<post>{IDENT}))?$")
_WORDOP_RE = re.compile(
    rf"^(?P<name>{DOTTED})\s+(?P<op>{'|'.join(WORD_OPERATORS)})\b(?P<rest>.*)$", re.S)


class ClassBlock:
    def __init__(self, name, kind, first_line, last_line, sections):
        self.name = name
        self.kind = kind                 # "CLASS" or "ALTERNATE"
        self.first_line = first_line
        self.last_line = last_line
        # [(keyword, text, first line, last line, absolute offset of text)]
        self.sections = sections


def find_class_blocks(src):
    """Split the class region into blocks and each block into sub-sections.

    Class headers and sub-section keywords may both start mid-line, after a `;`,
    so both are located by scanning the raw text.
    """
    heads = list(CLASS_HEADER_RE.finditer(src.text))
    blocks = []
    for k, m in enumerate(heads):
        end = heads[k + 1].start() if k + 1 < len(heads) else len(src.text)
        body_start = m.end()
        body = src.text[body_start:end]
        kind = "ALTERNATE" if m.group(1).startswith("ALTERNATE") else "CLASS"

        marks = []
        for off, word in scan_words(body):
            keyword = next((k for k in SUBSECTION_KEYWORDS if word.startswith(k)),
                           None)
            if keyword is None:
                continue
            before = body[:off].rstrip(" \t")
            # A sub-section keyword is accepted only at a statement boundary: the
            # start of the body, right after a `;`, at the start of a line, or
            # against the closing quote of the CONDITIONS message before it.
            if (before == "" or before.endswith(";") or before.endswith("\n")
                    or before.endswith('"')):
                marks.append((off, keyword))

        sections = []
        for j, (off, word) in enumerate(marks):
            nxt = marks[j + 1][0] if j + 1 < len(marks) else len(body)
            sections.append((
                word,
                body[off + len(word):nxt],
                src.line_of(body_start + off),
                src.line_of(body_start + nxt - 1),
                body_start + off + len(word),
            ))
        blocks.append(ClassBlock(
            m.group(2), kind, src.line_of(m.start()),
            src.line_of(end - 1), sections))
    return blocks


# ---------------------------------------------------------------------------
# FORMAT
# ---------------------------------------------------------------------------

def _restore_missing_separators(body, problems, where, declared):
    """Put back the space between two bindings that the description ran together."""
    while True:
        for m in BINDING_RE.finditer(body):
            if body[m.end():m.end() + 1] != ":":
                continue
            field = m.group("name")
            cut = next((k for k in range(1, len(field)) if field[k:] in declared),
                       None)
            if cut is None:
                continue
            problems.append(
                f"{where}: repaired missing separator in {m.group(0)!r}")
            at = m.start("name") + cut
            body = body[:at] + " " + body[at:]
            break
        else:
            return body


def parse_format(text, problems, where, declared):
    """Return (raw FORMAT text including keyword and `;`, {name: binding}).

    The block ends at its first `;` outside quotes, which may sit on the same line
    as the next sub-section keyword.
    """
    body, rest = split_at_semicolon(text)
    raw = "FORMAT" + body + (";" if rest is not None else "")
    if rest is None:
        problems.append(f"{where}: FORMAT block has no terminating ';'")

    # Two bindings occasionally run together with no separator between them:
    # sm_86 has `/ONLY32:szBD:barReg` and sm_89 has `/FMT("S32"):fmtRegister:Rd`.
    # The bound name ends where a declared type begins, so the separator is put
    # back into the text and both bindings are then read normally; cutting the
    # first name alone would drop the second binding and with it the second
    # field's type.
    body = _restore_missing_separators(body, problems, where, declared)

    bindings = {}
    for m in BINDING_RE.finditer(body):
        type_name = m.group("type")
        field = m.group("name")

        default = None
        args = m.group("args")
        if args is not None:
            # Argument grammar is `arg1[/arg2[/PRINT]]`. A leading
            # digit means a `<width>/<default>` spec, which only ever occurs on
            # the eight built-in immediate types and carries no symbolic default.
            head = args.split("/")[0].strip()
            if head and not head[0].isdigit():
                default = head[1:-1] if head.startswith('"') else head
        bindings[field] = {"type": type_name, "default": default}
    return raw, bindings


# ---------------------------------------------------------------------------
# OPCODES
# ---------------------------------------------------------------------------

# Mnemonics may carry a dot: `UIADD3.64`, `HFMA2.MMA`. Values are binary
# literals everywhere except the half-precision packed classes of sm_52..sm_62,
# which use plain decimal; no hexadecimal opcode value occurs.
_OPCODE_RE = re.compile(
    rf"^(?P<name>{MNEMONIC})\s*=\s*(?P<value>0[bB][01_]+|\d+)$")


def parse_opcodes(text, problems, where):
    """Return (mnemonic list, [opcode values], first value was decimal)."""
    names = []
    values = []
    first_decimal = False
    for _, stmt, _terminated in split_statements(text):
        stmt = " ".join(stmt.split())
        m = _OPCODE_RE.match(stmt)
        if not m:
            problems.append(f"{where}: unparsed OPCODES statement {stmt!r}")
            continue
        names.append(m.group("name"))
        value, decimal = parse_int_literal(m.group("value"))
        if not values:
            first_decimal = decimal
        if value not in values:
            values.append(value)
    if len(values) > 1:
        problems.append(f"{where}: OPCODES lists {len(values)} distinct values; "
                        f"using the first for the Opcode slot")
    return names, values, first_decimal


# ---------------------------------------------------------------------------
# PROPERTIES
# ---------------------------------------------------------------------------

_PROPERTY_RE = re.compile(rf"^(?P<name>{IDENT})\s*(?:=\s*(?P<value>.*))?$", re.S)


def parse_properties(text, problems, where, into):
    """Fill `into` with {property name: raw value string}.

    Two statement forms occur: `NAME = <expr>` and a bare `NAME` flag. Flags get
    an empty value. A section-level `PROPERTIES SECONDARY;` flag
    reaches here as the first statement and is stored the same way.
    """
    for _, stmt, _terminated in split_statements(text):
        stmt = " ".join(stmt.split())
        m = _PROPERTY_RE.match(stmt)
        if not m:
            problems.append(f"{where}: unparsed PROPERTIES statement {stmt!r}")
            continue
        into[m.group("name")] = (m.group("value") or "").strip()


# ---------------------------------------------------------------------------
# ENCODING
# ---------------------------------------------------------------------------

def _slot_tag(slot):
    """Name used when synthesising a key for an unnamed (constant) slot value.

    For a sm_70+ slot the tag embedded in the name is used; for the sm_50..sm_62
    dialect the slot name itself is the only thing available.
    """
    decoded = bits_from_slot_name(slot)
    return decoded[1] if decoded else slot


def _parse_rhs(rhs, problems, where):
    """Classify an ENCODING right-hand side.

    Returns a dict describing one of the four kinds, or None when nothing matches.
    """
    rhs = rhs.strip()

    # `X SCALE 4`, `X MULTIPLY 4 SCALE 4`, `X convertFloatType(...)`.
    m = _WORDOP_RE.match(rhs)
    if m:
        extra = {}
        op, rest = m.group("op"), m.group("rest").strip()
        if op == "convertFloatType":
            # Argument list of a three- or five-argument call, kept as its
            # top-level arguments rather than one blob.
            extra["convert_float_type"] = _split_call_args(rest)
        elif op == "SCALE":
            extra["scale"] = int(rest)
        else:  # MULTIPLY is always followed by SCALE in the corpus
            parts = rest.split()
            extra["multiply"] = int(parts[0])
            if len(parts) >= 3 and parts[1] == "SCALE":
                extra["scale"] = int(parts[2])
            else:
                problems.append(f"{where}: MULTIPLY without SCALE in {rhs!r}")
        return {"kind": "field", "name": m.group("name"), "post": None, "extra": extra}

    # Table or built-in call.
    m = _CALL_RE.match(rhs)
    if m:
        args = [a.strip() for a in m.group("args").split(",")] if m.group("args").strip() else []
        return {"kind": "call", "call_name": m.group("name"), "args": args,
                "text": m.group("name") + "(" + ",".join(args) + ")"}

    # `\`Type@VALUE` enum reference; sm_50..sm_62 only.
    m = _ENUMREF_RE.match(rhs)
    if m:
        value = m.group("value")
        return {"kind": "enumref", "type": m.group("type"),
                "value": value[1:-1] if value.startswith('"') else value}

    literal = parse_int_literal(rhs)
    if literal is not None:
        return {"kind": "constant", "value": literal[0], "decimal": literal[1]}

    m = _FIELD_RE.match(rhs)
    if m:
        return {"kind": "field", "name": m.group("name"),
                "post": m.group("post"), "extra": {}}

    return None


def _split_call_args(text):
    """Split a parenthesised argument list on its top-level commas.

    The first argument is a condition expression, so the split tracks depth
    rather than cutting on every comma. No argument list in the corpus
    actually nests, so the two agree on every input seen so far.
    """
    body = text.strip()
    if body.startswith("(") and body.endswith(")"):
        body = body[1:-1]
    args, depth, cur = [], 0, ""
    for ch in body:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            args.append(cur.strip())
            cur = ""
        else:
            cur += ch
    args.append(cur.strip())
    return args


def _low_bit(slot_name):
    """Lowest bit position a slot occupies, used to tell same-tag slots apart."""
    decoded = bits_from_slot_name(slot_name)
    return min(decoded[0]) if decoded and decoded[0] else 0


def _unique_constant_key(slot_values, tag, slot_name):
    """Pick a key for a constant, keeping `__const_<tag>__` where it is free.

    Two slots inside one class can share a tag and hold different constants.
    They are told apart by the low bit of the slot each occupies, which is what
    actually distinguishes them.
    """
    plain = f"__const_{tag}__"
    if plain in slot_values:
        # Tag the one already there as well, so that neither name depends on
        # which assignment the source happened to list first.
        prev = slot_values.pop(plain)
        slot_values[f"__const_{tag}@{_low_bit(prev['bit_slot'][0])}__"] = prev
    elif not any(k.startswith(f"__const_{tag}@") for k in slot_values):
        return plain
    return f"__const_{tag}@{_low_bit(slot_name)}__"


def _merge(slot_values, key, entry, slot, starred, problems, where):
    """Add or merge one encoded item.

    A pair left-hand side calls this once per slot with the same key, which is
    why `bit_slot` is a list.
    """
    existing = slot_values.get(key)
    if existing is None:
        entry["bit_slot"] = [slot]
        entry["is_starred"] = starred
        slot_values[key] = entry
        return
    existing["bit_slot"].append(slot)
    if existing["is_starred"] != starred:
        problems.append(f"{where}: {key!r} has conflicting '*' markers; taking OR")
        existing["is_starred"] = True


def parse_encoding(text, offset, src, problems, where, bindings, opcode_value,
                   opcode_is_decimal, enum_lookup, stats):
    """Build the `slot_values` map for one class."""
    slot_values = {}
    slots_used = []
    # Index of the class's bindings by type, for the type-name right-hand sides
    # of the sm_50..sm_62 dialect.
    by_type = {}
    for binding in bindings.values():
        by_type.setdefault(binding["type"], []).append(binding)

    for stmt_offset, stmt, terminated in split_statements(text, offset):
        line = src.line_of(stmt_offset)
        loc = f"{where} line {line}"
        stmt = " ".join(stmt.split())
        if not terminated:
            # The file ends inside this statement, so what follows the last
            # character read is unknown.
            problems.append(f"{loc}: statement is not terminated; the file ends "
                            f"inside {stmt!r}")
            continue

        # REMAP carries '=' inside its quoted template, so it has to be
        # recognised before any assignment split.
        if stmt.startswith("REMAP"):
            stats["remap"] += 1
            continue

        # `!name;` names a slot without giving it a value, so it produces no
        # encoded item.
        if stmt.startswith("!"):
            stats["unused"] += 1
            continue

        if "=" not in stmt:
            problems.append(f"{loc}: unparsed ENCODING statement {stmt!r}")
            continue

        lhs, rhs = stmt.split("=", 1)
        starred = rhs.lstrip().startswith("*")
        if starred:
            rhs = rhs.lstrip()[1:]
        slots = [s.strip() for s in lhs.split(",")]
        if len(slots) > 2:
            problems.append(f"{loc}: left-hand side with {len(slots)} names")
            continue
        slots_used.extend(slots)
        stats["assignments"] += 1

        parsed = _parse_rhs(rhs, problems, loc)
        if parsed is None:
            problems.append(f"{loc}: unparsed right-hand side {rhs.strip()!r}")
            continue

        if parsed["kind"] == "call":
            entry = {"kind": "call", "call_name": parsed["call_name"], "args": parsed["args"]}
            key = parsed["text"]
        elif parsed["kind"] == "constant":
            entry = {"kind": "constant", "value": parsed["value"]}
            if not parsed["decimal"]:
                # Carried only for values the source did not spell in decimal.
                # Binary and hexadecimal both come back as hexadecimal, so this
                # records the distinction, not the original spelling.
                entry["value_hex"] = hex(parsed["value"])
            key = _unique_constant_key(slot_values, _slot_tag(slots[0]), slots[0])
        elif parsed["kind"] == "enumref":
            values = enum_lookup.get(parsed["type"])
            if values is None or parsed["value"] not in values:
                problems.append(f"{loc}: unresolved enum reference "
                                f"`{parsed['type']}@{parsed['value']}")
                continue
            entry = {"kind": "constant", "value": values[parsed["value"]]}
            key = _unique_constant_key(slot_values, _slot_tag(slots[0]), slots[0])
        else:
            name, post = parsed["name"], parsed["post"]
            if post is not None and post not in POST_OPERATORS:
                problems.append(f"{loc}: unknown post-operator @{post} in {rhs.strip()!r}")
                continue
            if name == "Opcode":
                # The bare `Opcode` placeholder resolves to this class's opcode
                # value.
                if opcode_value is None:
                    problems.append(f"{loc}: Opcode used but no OPCODES value")
                    continue
                entry = {"kind": "constant", "value": opcode_value}
                if not opcode_is_decimal:
                    entry["value_hex"] = hex(opcode_value)
                key = "Opcode"
            else:
                entry = {"kind": "field"}
                key = f"{name}@{post}" if post else name
                if post is None:
                    # format_type / format_default come from the FORMAT binding,
                    # and only when the type is declared.
                    binding = bindings.get(name)
                    if binding is None:
                        # The sm_50..sm_62 dialect names some fields on the
                        # right-hand side by their type rather than by the bound
                        # name (`Rnd = Round1;`). Every such reference resolves
                        # to exactly one binding of that type in the same class.
                        same_type = by_type.get(name, ())
                        if len(same_type) == 1:
                            binding = same_type[0]
                    if binding and binding["type"] in enum_lookup:
                        entry["format_type"] = binding["type"]
                        if binding["default"] is not None:
                            entry["format_default"] = binding["default"]
                entry.update(parsed["extra"])

        # A pair left-hand side yields one item spanning two slots.
        for slot in slots:
            _merge(slot_values, key, dict(entry), slot, starred, problems, loc)

    ordered = {}
    for key, entry in slot_values.items():
        out = {"kind": entry["kind"]}
        if entry["kind"] == "constant":
            out["value"] = entry["value"]
            if "value_hex" in entry:
                out["value_hex"] = entry["value_hex"]
        elif entry["kind"] == "call":
            out["call_name"] = entry["call_name"]
            out["args"] = entry["args"]
        out["is_starred"] = entry["is_starred"]
        for extra in ("format_type", "format_default", "scale", "multiply",
                      "convert_float_type"):
            if extra in entry:
                out[extra] = entry[extra]
        out["bit_slot"] = entry["bit_slot"]
        ordered[key] = out
    return ordered, slots_used
