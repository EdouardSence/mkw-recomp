"""Lexical primitives for NVIDIA SIDL instruction dumps.

The constructs these rules handle are described in docs/sidl_corpus_survey.md.

Three facts about the corpus drive the whole design:

  * There are no comments, no line continuations and no blank lines,
    so raw text can be scanned directly.
  * Indentation carries no structure below the region level, so the
    only reliable delimiters are keywords, `;` and quotes.
  * A `;`-terminated statement, a sub-section keyword and even a class header can
    all start in the middle of a physical line, so line-oriented
    splitting is wrong and token scanning is required.
"""

import re

# Region keywords that start in column 0. `OPERATION` carries a second
# word; `TABLES` is always alone on its line.
SIMPLE_REGION_KEYWORDS = (
    "PARAMETERS",
    "CONSTANTS",
    "STRING_MAP",
    "REGISTERS",
    "NOP_ENCODING",
)

# Class sub-section keywords. A keyword is matched as a prefix of the scanned
# word, because the description sometimes runs one into the token after it
# (`CONDITIONSOOR_REG_ERROR`), so a longer spelling has to come first.
SUBSECTION_KEYWORDS = (
    "FORMAT_ALIAS",
    "FORMAT",
    "CONDITIONS",
    "CONDITION",
    "PROPERTIES",
    "PREDICATES",
    "OPCODES",
    "ENCODING",
)

# Both class header spellings. The quoted name is part of the match, so a bare
# `CLASS` word does not start a block.
CLASS_HEADER_RE = re.compile(r'\b(ALTERNATE\s+CLASS|CLASS)\s+"([^"]*)"')

IDENT = r"[A-Za-z_][A-Za-z_0-9]*"
# Slot and field names may carry a dot: `BITS_4_85_84_76_75_dstfmt.srcfmt`,
# `/F2Ffmts2:dstfmt.srcfmt`.
DOTTED = r"[A-Za-z_][A-Za-z_0-9]*(?:\.[A-Za-z_][A-Za-z_0-9]*)*"
# Opcode mnemonics may carry dotted components of letters and digits:
# `UIADD3.64`, `UMOV.64udp_pipe`, `HFMA2.MMA`.
MNEMONIC = r"[A-Za-z_][A-Za-z_0-9]*(?:\.[A-Za-z_0-9]+)*"


class SourceFile:
    """A dump held as text plus a line index, with per-line ownership tracking."""

    def __init__(self, path):
        with open(path, encoding="utf-8", errors="replace") as fh:
            self.text = fh.read()
        self.lines = self.text.split("\n")
        # Absolute offset at which each line starts, for offset -> line lookups.
        self.line_start = [0]
        for i, ch in enumerate(self.text):
            if ch == "\n":
                self.line_start.append(i + 1)
        self.owner = [None] * len(self.lines)

    def line_of(self, offset):
        """1-based line number containing `offset`."""
        lo, hi = 0, len(self.line_start) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.line_start[mid] <= offset:
                lo = mid
            else:
                hi = mid - 1
        return lo + 1

    def claim(self, first_line, last_line, label):
        """Label lines [first_line, last_line] (1-based, inclusive)."""
        for i in range(first_line - 1, min(last_line, len(self.lines))):
            if self.owner[i] is None:
                self.owner[i] = label

    def unclaimed(self):
        return [(i + 1, self.lines[i]) for i, o in enumerate(self.owner)
                if o is None and self.lines[i].strip()]


def scan_words(text):
    """Yield (offset, word) for every identifier outside single/double quotes.

    There are no escape sequences inside strings anywhere in the corpus, so a
    plain scan to the next matching quote is correct.
    """
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch in "'\"":
            quote = ch
            i += 1
            while i < n and text[i] != quote:
                i += 1
            i += 1
            continue
        if ch.isalpha() or ch == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            yield i, text[i:j]
            i = j
            continue
        i += 1


def split_statements(text, base_offset=0):
    """Split on `;` outside quotes into (offset, statement, terminated) triples.

    `terminated` is False only for a trailing statement the file ends inside.
    Used for the `;`-terminated, non-line-oriented sections: ENCODING,
    PROPERTIES and OPCODES.
    """
    out = []
    buf = []
    start = 0
    quote = None
    for i, ch in enumerate(text):
        if quote:
            buf.append(ch)
            if ch == quote:
                quote = None
            continue
        if ch in "'\"":
            quote = ch
            buf.append(ch)
            continue
        if ch == ";":
            raw = "".join(buf)
            s = raw.strip()
            if s:
                lead = len(raw) - len(raw.lstrip())
                out.append((base_offset + start + lead, s, True))
            buf = []
            start = i + 1
            continue
        buf.append(ch)
    raw_tail = "".join(buf)
    tail = raw_tail.strip()
    if tail:
        # Some files are truncated mid-statement. The tail is kept
        # and flagged as unterminated so the caller can say so instead of
        # treating a half statement as a whole one.
        out.append((base_offset + start + len(raw_tail) - len(raw_tail.lstrip()),
                    tail, False))
    return out


def split_at_semicolon(text):
    """Return (before, after) at the first `;` outside quotes, or (text, None).

    FORMAT is delimited this way: it runs to its first unquoted `;`, which may sit
    on the same line as the next sub-section keyword.
    """
    quote = None
    for i, ch in enumerate(text):
        if quote:
            if ch == quote:
                quote = None
            continue
        if ch in "'\"":
            quote = ch
            continue
        if ch == ";":
            return text[:i], text[i + 1:]
    return text, None


def parse_int_literal(tok):
    """Parse the three integer literal spellings that occur.

    Returns (value, is_decimal) or None. Underscores appear inside binary literals
    (`0b0011010_1`) and must be dropped.
    """
    t = tok.strip()
    if re.fullmatch(r"0[bB][01_]+", t):
        return int(t[2:].replace("_", ""), 2), False
    if re.fullmatch(r"0[xX][0-9a-fA-F_]+", t):
        return int(t[2:].replace("_", ""), 16), False
    if re.fullmatch(r"\d+", t):
        return int(t), True
    return None


BITS_NAME_RE = re.compile(r"^BITS_(\d+)_(.*)$")


def bits_from_slot_name(name):
    """Decode a `BITS_<width>_<hi>_<lo>..._<tag>` slot name.

    The tag may contain digits and underscores, so ranges are consumed greedily
    left to right until the accumulated bit count equals the declared width. That
    rule reproduces every `BITS_` name in the corpus with no failures.

    Returns (sorted bit positions, tag, width) or None when the name is not of
    this form, as in the sm_50..sm_62 dialect.
    """
    m = BITS_NAME_RE.match(name)
    if not m:
        return None
    width = int(m.group(1))
    parts = m.group(2).split("_")
    bits = []
    i = 0
    while i + 1 < len(parts) and len(bits) < width:
        hi_s, lo_s = parts[i], parts[i + 1]
        if not (hi_s.isdigit() and lo_s.isdigit()):
            break
        hi, lo = int(hi_s), int(lo_s)
        if hi < lo:
            break
        span = list(range(lo, hi + 1))
        if len(bits) + len(span) > width:
            break
        bits.extend(span)
        i += 2
        if len(bits) == width:
            break
    if len(bits) != width:
        return None
    return sorted(bits), "_".join(parts[i:]), width


def bits_from_mask(mask, encoding_width):
    """Decode a FUNIT mask string into absolute bit positions.

    Masks are MSB-first: character index i is bit `width-1-i`. Verified against
    `Dest` in sm_50 (last eight characters -> bits 0..7) and against every
    `BITS_` name in the sm_70+ dialect.
    """
    return sorted(encoding_width - 1 - i for i, c in enumerate(mask) if c == "X")
