"""Decoding model built from `data/<arch>/instructions.json`.

This module turns the parsed instruction description into the form a decoder
needs: per class, a list of constraints over the 128-bit word, plus the
information needed to read the fields back out and to write them back in.

The reasoning is written out in full in `docs/decoding_rule.md`.
"""

import json
import os

WORD_BITS = 128


class Slot:
    """One `FUNIT` bit slot.

    `positions` is the catalog's ascending bit list. Reading it as
    "element *i* is field bit *i*" is the same thing as concatenating the
    name's ranges most-significant group first, because the ranges of a name
    are always listed in descending, non-overlapping order (the cross-check in
    `docs/decoding_rule.md`).
    """

    __slots__ = ("name", "positions", "width", "mask")

    def __init__(self, name, positions):
        self.name = name
        self.positions = positions
        self.width = len(positions)
        mask = 0
        for p in positions:
            mask |= 1 << p
        self.mask = mask

    def extract(self, word):
        v = 0
        for i, p in enumerate(self.positions):
            v |= ((word >> p) & 1) << i
        return v

    def deposit(self, word, value):
        for i, p in enumerate(self.positions):
            if (value >> i) & 1:
                word |= 1 << p
            else:
                word &= ~(1 << p)
        return word


class Item:
    """One `ENCODING` assignment, as the decoder sees it.

    `kind` is the parser's kind (`constant`, `field`, `call`). `slots` is the
    list of bit slots the value is written to; the pair form `A,B = Call(x,y)`
    is the one that writes two.
    """

    __slots__ = ("name", "kind", "value", "is_starred", "slots", "format_type",
                 "call_name", "allowed")

    def __init__(self, name, entry, slots, allowed):
        self.name = name
        self.kind = entry["kind"]
        self.value = entry.get("value")
        self.is_starred = bool(entry.get("is_starred"))
        self.slots = slots
        self.format_type = entry.get("format_type")
        self.call_name = entry.get("call_name")
        # None means "any value the slot can hold"; otherwise a set of ints.
        self.allowed = allowed


class InstructionClass:
    def __init__(self, name, raw, slots_by_name, enum_tables, tables,
                 is_alternate=False):
        self.name = name
        self.is_alternate = is_alternate
        self.mnemonic = raw["mnemonic"]
        self.items = []
        self.opcode = None
        covered = 0
        for item_name, entry in raw["slot_values"].items():
            slots = [slots_by_name[s] for s in entry["bit_slot"]]
            allowed = _allowed_values(entry, enum_tables, tables)
            item = Item(item_name, entry, slots, allowed)
            self.items.append(item)
            for s in slots:
                covered |= s.mask
            if item_name == "Opcode" and item.kind == "constant":
                self.opcode = item
        # Bits the class does not assign. Verified to be exactly the class's
        # `!<name>_unused` FUNIT mask for all 6 940 class blocks of the five
        # architectures (see docs/decoding_rule.md).
        self.covered_mask = covered
        self.unused_mask = ((1 << WORD_BITS) - 1) & ~covered


def _allowed_values(entry, enum_tables, tables):
    """The set of values this assignment can put into its slot, or None for
    "unconstrained".

    * `constant` -- exactly the one value. The `ENCODING` statement writes a
      literal, so no other value can come out of the assembler for this class.
    * `field` with a declared type -- the values that type enumerates. A
      binding's type either resolves to a declaration (a register group or a
      modifier enumeration; the text does not separate the two) or it is one of
      the seven immediate and bitset built-ins. A declared type is a closed set of
      symbols, so the field cannot hold anything else.
    * `field` with a built-in type -- any value (an immediate).
    * `call` -- the outputs the table can produce. Many classes assert
      `DEFINED <TableCall>(...)` in their `CONDITIONS`, i.e. the argument tuple
      must be a row of the table, so the slot can only hold an output the table
      lists. Not all of them do; see docs/decoding_rule.md.
    """
    kind = entry["kind"]
    if kind == "constant":
        return {int(entry["value"])}
    if kind == "field":
        ftype = entry.get("format_type")
        if ftype and ftype in enum_tables:
            return set(enum_tables[ftype]["values"].values())
        return None
    if kind == "call":
        table = tables.get(entry.get("call_name"))
        if table and table.get("decode"):
            values = set()
            for key in table["decode"]:
                try:
                    values.add(int(key, 0))
                except ValueError:
                    # A row whose output did not resolve to an integer leaves
                    # the output set incomplete, so using it as a constraint
                    # could reject a valid word. Drop the constraint instead.
                    return None
            return values
        return None
    return None


class Spec:
    """The decoding model for one architecture."""

    def __init__(self, path):
        with open(path, "r", encoding="utf-8") as handle:
            self.doc = json.load(handle)
        self.path = path
        self.meta = self.doc["meta"]
        self.enum_tables = self.doc["enum_tables"]
        self.tables = self.doc["tables"]

        self.slots = {}
        for name, info in self.doc["bit_slot_catalog"].items():
            self.slots[name] = Slot(name, info["bit_positions"])

        # Primary and `ALTERNATE CLASS` blocks both encode instruction words,
        # so both are loaded and both are matched against.
        self.classes = []
        self.alternate_count = 0
        for name, raw in self.doc["instructions"].items():
            is_alternate = bool(raw.get("is_alternate"))
            if is_alternate:
                self.alternate_count += 1
            self.classes.append(InstructionClass(
                name, raw, self.slots, self.enum_tables, self.tables,
                is_alternate=is_alternate))

        # Every class of a supported architecture carries exactly one `Opcode`
        # constant, in the same slot, never starred. That slot is the index.
        # sm_75 breaks it: some classes take the opcode from bits 0..11 and the
        # rest from bits 0..11 plus bit 91. sm_50..sm_62 are a different encoding
        # altogether, 88 bits wide with hand-written slot names. The check below
        # states that rather than failing somewhere further in.
        without = [c.name for c in self.classes if not c.opcode]
        if without:
            raise ValueError(f"{path}: {len(without)} classes carry no Opcode "
                             f"constant, starting with {without[0]!r}")
        opcode_slots = {tuple(s.name for s in c.opcode.slots)
                        for c in self.classes}
        if len(opcode_slots) != 1:
            raise ValueError(
                f"{path}: this architecture places the opcode in more than one "
                f"layout ({sorted(opcode_slots)}), which the decoder does not "
                f"support")
        self.opcode_slots = [self.slots[n] for n in next(iter(opcode_slots))]

        self.by_opcode = {}
        for c in self.classes:
            self.by_opcode.setdefault(c.opcode.value, []).append(c)

    def opcode_of(self, word):
        return self.opcode_slots[0].extract(word)


def load(arch, root=None):
    root = root or os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
    return Spec(os.path.join(root, arch, "instructions.json"))
