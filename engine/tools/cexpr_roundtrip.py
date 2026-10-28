#!/usr/bin/env python3
"""An INDEPENDENT soundness check on tools/cexpr.py's parse trees.

Why this file exists, and why it is a separate file
---------------------------------------------------
`tools/semantic_equivalence.py` proves that a rewrite computes what the
original computed by re-parsing BOTH spellings with `cexpr` and comparing the
trees.  That defends against splicing bugs.  It CANNOT defend against the
parser itself being wrong, because a systematic mis-parse agrees with itself:
if `cexpr` reads `(MACRO * (a/b)) + c` as a CAST of `+c`, it reads the rewrite
the same way, the trees match, and the verdict is a confident YES over a tree
that has nothing to do with the C.

So the check here must not share the parser's assumptions.  It has two halves,
and neither reuses `Parser._is_type_name`:

  1. STRUCTURAL UNPARSE + ROUND TRIP.  `unparse()` rebuilds source from the
     tree's SHAPE -- kind, operator, precedence -- and never slices the input
     (leaf spellings and type-names are the only text that survives, and
     type-names go through half 2).  Three properties are then required:
       (a) the non-paren token SEQUENCE of the unparse equals the input's, so
           nothing was dropped, duplicated or re-ordered;
       (b) re-parsing the unparse and unparsing again is IDEMPOTENT, so the
           tree the parser builds for its own canonical spelling is the tree
           it built for the original;
       (c) parens the grammar needs are re-emitted from precedence alone, so a
           normaliser can never silently erase a load-bearing paren.
  2. AN INDEPENDENT TYPE-NAME GRAMMAR.  Every `cast` (and every type-shaped
     `sizeof`) carries a raw slice of source as its type.  `canonical_type()`
     parses that slice against a real abstract-declarator grammar written from
     C's syntax, not from the heuristic that produced it.  A cast whose type
     is not a type-name is a MIS-PARSE, and this is the half that catches the
     macro class: `SHIELD_SFX_VOLUME_MAX * (state->a / state->b)` is a
     perfectly good multiplication and not a type-name in any C.

Property (a) alone does NOT catch the cast mutation -- the mis-parse preserves
every token.  Half 2 alone does not catch a re-association.  Both are needed,
which is why both are here and why `--self-test` reports each separately.

Run `python3 tools/cexpr_roundtrip.py --self-test` for the controls, and
`--scan` to sweep every expression the project's own sweeper reaches.
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass, field
from typing import List, Optional, Sequence, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cexpr
from cexpr import (Ambiguous, BASE_TYPE_WORDS, Node, ParseError,
                   parse_expression, tokenize)


class RoundTripError(Exception):
    """The tree does not describe the source it was built from."""


class NotATypeName(RoundTripError):
    """A cast/sizeof type slice is not a type-name in C."""


# --------------------------------------------------------------- type-names

# The core type words -- the ones that can stand alone as a type.  `const`,
# `volatile`, `struct`, `union` and `enum` are NOT here: they qualify or
# introduce, they are not themselves a type.
_QUALIFIERS = {"const", "volatile"}
_TAG_KEYWORDS = {"struct", "union", "enum"}
_CORE_WORDS = BASE_TYPE_WORDS - _QUALIFIERS - _TAG_KEYWORDS
# `unsigned x` / `long long` / `signed char`: several core words may combine.
_COMBINING = {"signed", "unsigned", "short", "long", "int", "char", "double"}


class _TypeNameParser:
    """C's type-name production, written straight from the grammar.

    type-name        := specifier-qualifier-list abstract-declarator?
    abstract-decl    := pointer | pointer? direct-abstract-declarator
    direct-abs-decl  := '(' abstract-declarator ')' suffix*
                      | suffix+
    suffix           := '[' constant? ']' | '(' parameter-list? ')'

    Deliberately written without reference to `cexpr.Parser._is_type_name`:
    that routine scans a token shape and asks only whether a `)` lands where
    it expects.  This one has to actually consume a declarator.
    """

    def __init__(self, toks: Sequence[cexpr.Tok]):
        self.t = list(toks)
        self.i = 0
        self.depth = 0
        # True once an UNGROUPED `(params)` or `[n]` suffix has been consumed
        # at the outermost level, i.e. the type-name denotes a function or an
        # array rather than a pointer to one.  `sizeof` may take such a type;
        # a CAST may not, and C forbids it outright.
        self.bare_suffix = ""

    # -- helpers
    def peek(self, d: int = 0) -> cexpr.Tok:
        j = self.i + d
        return self.t[j] if j < len(self.t) else self.t[-1]

    def at_punct(self, text: str, d: int = 0) -> bool:
        c = self.peek(d)
        return c.kind == "punct" and c.text == text

    def take(self) -> cexpr.Tok:
        c = self.peek()
        self.i += 1
        return c

    def expect(self, text: str) -> None:
        if not self.at_punct(text):
            raise NotATypeName("expected %r got %r" % (text, self.peek().text))
        self.i += 1

    # -- specifier-qualifier-list
    def spec_qual_list(self) -> List[str]:
        out: List[str] = []
        core = False
        while True:
            c = self.peek()
            if c.kind != "id":
                break
            tx = c.text
            if tx in _QUALIFIERS:
                out.append(tx)
                self.i += 1
                continue
            if tx in _TAG_KEYWORDS:
                if core:
                    break
                out.append(tx)
                self.i += 1
                tag = self.peek()
                if tag.kind != "id":
                    raise NotATypeName("%s without a tag name" % tx)
                out.append(tag.text)
                self.i += 1
                core = True
                continue
            if tx in _CORE_WORDS:
                # `unsigned long int` combines; `f32 Vec3f` does not.
                if core and not (tx in _COMBINING and out[-1] in _COMBINING):
                    break
                out.append(tx)
                self.i += 1
                core = True
                continue
            if not core:
                # a typedef name.  Exactly one is allowed, and it may not be
                # followed by another bare identifier.
                out.append(tx)
                self.i += 1
                core = True
                continue
            break
        if not core:
            raise NotATypeName("no type specifier in %r" %
                               " ".join(t.text for t in self.t[:-1]))
        return out

    # -- pointer
    def pointer(self) -> List[str]:
        out: List[str] = []
        while self.at_punct("*"):
            out.append("*")
            self.i += 1
            while self.peek().kind == "id" and self.peek().text in _QUALIFIERS:
                out.append(self.take().text)
        return out

    # -- suffixes
    def suffixes(self) -> List[str]:
        out: List[str] = []
        while True:
            if self.at_punct("["):
                self.i += 1
                out.append("[")
                while not self.at_punct("]"):
                    c = self.take()
                    if c.kind == "eof":
                        raise NotATypeName("unterminated `[`")
                    out.append(c.text)
                self.i += 1
                out.append("]")
                continue
            if self.at_punct("("):
                self.i += 1
                out.append("(")
                self.depth += 1
                out.extend(self.parameter_list())
                self.depth -= 1
                self.expect(")")
                out.append(")")
                continue
            return out

    def parameter_list(self) -> List[str]:
        out: List[str] = []
        if self.at_punct(")"):
            return out
        if self.peek().kind == "id" and self.peek().text == "void" and \
                self.at_punct(")", 1):
            self.i += 1
            return ["void"]
        while True:
            if self.at_punct("..."):
                self.i += 1
                out.append("...")
            else:
                out.extend(self.type_name())
            if self.at_punct(","):
                self.i += 1
                out.append(",")
                continue
            return out

    # -- declarators
    def direct_abstract_declarator(self) -> List[str]:
        out: List[str] = []
        if self.at_punct("("):
            # `(` here is a grouped declarator only if what follows can start
            # one; otherwise it is a parameter list on an unnamed function.
            nxt = self.peek(1)
            grouped = (nxt.kind == "punct" and nxt.text in ("*", "(")) or \
                      (nxt.kind == "punct" and nxt.text == "[")
            if grouped:
                self.i += 1
                out.append("(")
                self.depth += 1
                out.extend(self.abstract_declarator())
                self.depth -= 1
                self.expect(")")
                out.append(")")
                out.extend(self.suffixes())
                return out
        here = self.i
        sfx = self.suffixes()
        if sfx and self.depth == 0 and not self.bare_suffix:
            self.bare_suffix = "function" if self.t[here].text == "(" else "array"
        out.extend(sfx)
        return out

    def abstract_declarator(self) -> List[str]:
        out = self.pointer()
        rest = self.direct_abstract_declarator()
        if not out and not rest:
            raise NotATypeName("empty abstract declarator")
        out.extend(rest)
        return out

    def type_name(self) -> List[str]:
        out = self.spec_qual_list()
        if self.at_punct("*") or self.at_punct("(") or self.at_punct("["):
            out.extend(self.abstract_declarator())
        return out

    def parse(self, as_cast: bool = True) -> List[str]:
        # A parameter list is parsed at depth+1 so that a function type-name
        # nested inside a declarator does not read as the outer one.
        out = self.type_name()
        if self.peek().kind != "eof":
            raise NotATypeName("trailing %r in type-name" % self.peek().text)
        if as_cast and self.bare_suffix:
            raise NotATypeName("cannot cast to a %s type" % self.bare_suffix)
        return out


def canonical_type(text: str, as_cast: bool = True) -> List[str]:
    """Token list for `text` read as a C type-name, or raise NotATypeName.

    This is the half of the check that catches a cast that was never a cast.
    `as_cast` additionally enforces C's rule that a cast's target may not be a
    function or array type -- which is what separates the genuine type-name
    `Vec3f (f32)` (legal under `sizeof`) from the mis-parse it would be inside
    a cast.
    """
    try:
        toks = tokenize(text)
    except ParseError as e:
        raise NotATypeName("untokenizable type %r: %s" % (text, e))
    if len(toks) <= 1:
        raise NotATypeName("empty type-name")
    return _TypeNameParser(toks).parse(as_cast)


# ------------------------------------------------------------------ unparse

# Higher binds tighter.  This table is written out from C's grammar and is
# DELIBERATELY NOT `cexpr.BIN_PREC`.  Importing the parser's table would make
# the round trip agree with the parser by construction on exactly the question
# the round trip exists to ask -- the `--mutate assoc` control caught that
# mistake in this file's first draft: with a flattened BIN_PREC shared by both,
# 57258 regions came back clean while every tree in them was wrong.
_BIN_PREC = {
    "*": 13, "/": 13, "%": 13,
    "+": 12, "-": 12,
    "<<": 11, ">>": 11,
    "<": 10, ">": 10, "<=": 10, ">=": 10,
    "==": 9, "!=": 9,
    "&": 8,
    "^": 7,
    "|": 6,
    "&&": 5,
    "||": 4,
}

P_PRIMARY = 100
P_POSTFIX = 90
P_UNARY = 80
P_COND = 3
P_ASSIGN = 2
P_COMMA = 1


def _prec(n: Node) -> int:
    # Look THROUGH a `paren` node.  Its own precedence is irrelevant -- the
    # unparse drops it and re-derives grouping, so what matters is the binding
    # strength of what it wraps.  Reading the paren's precedence instead is
    # exactly how a normaliser erases a load-bearing paren, and it is the bug
    # the MUTATION B control exists to keep out.
    while n.kind == "paren":
        n = n.kids[0]
    k = n.kind
    if k in ("id", "num", "str", "chr"):
        return P_PRIMARY
    if k in ("call", "index", "member", "postincr"):
        return P_POSTFIX
    if k in ("unary", "cast", "sizeof"):
        return P_UNARY
    if k == "bin":
        return _BIN_PREC[n.op]
    if k == "cond":
        return P_COND
    if k == "assign":
        return P_ASSIGN
    if k == "comma":
        return P_COMMA
    raise RoundTripError("unparse: unknown node kind %r" % k)


def _wrap(n: Node, need: int) -> List[str]:
    toks = unparse_tokens(n)
    if _prec(n) < need:
        return ["("] + toks + [")"]
    return toks


def unparse_tokens(n: Node) -> List[str]:
    """Rebuild the token stream from the tree's SHAPE.

    Nothing here reads `n.start`/`n.end`: if the tree is wrong the output is
    wrong in a way the caller can see.  Parentheses are re-derived from
    precedence, so a `paren` node is dropped and re-created only where the
    grammar requires it -- which is what makes a dropped load-bearing paren
    impossible rather than merely unlikely.
    """
    k = n.kind
    if k in ("id", "num", "str", "chr"):
        return [n.text]
    if k == "paren":
        return unparse_tokens(n.kids[0])
    if k == "bin":
        p = _BIN_PREC[n.op]
        return _wrap(n.kids[0], p) + [n.op] + _wrap(n.kids[1], p + 1)
    if k == "unary":
        return [n.op] + _wrap(n.kids[0], P_UNARY)
    if k == "postincr":
        return _wrap(n.kids[0], P_POSTFIX) + [n.op]
    if k == "cast":
        return ["("] + canonical_type(n.text) + [")"] + \
            _wrap(n.kids[0], P_UNARY)
    if k == "sizeof":
        if n.kids:
            return ["sizeof"] + _wrap(n.kids[0], P_UNARY)
        return ["sizeof", "("] + _sizeof_operand(n.text) + [")"]
    if k == "member":
        return _wrap(n.kids[0], P_POSTFIX) + [n.op, n.text]
    if k == "index":
        return _wrap(n.kids[0], P_POSTFIX) + ["["] + \
            unparse_tokens(n.kids[1]) + ["]"]
    if k == "call":
        out = _wrap(n.kids[0], P_POSTFIX) + ["("]
        for j, a in enumerate(n.kids[1:]):
            if j:
                out.append(",")
            out.extend(_wrap(a, P_ASSIGN))
        return out + [")"]
    if k == "cond":
        return _wrap(n.kids[0], P_COND + 1) + ["?"] + \
            unparse_tokens(n.kids[1]) + [":"] + _wrap(n.kids[2], P_COND)
    if k == "assign":
        return _wrap(n.kids[0], P_UNARY) + [n.op] + _wrap(n.kids[1], P_ASSIGN)
    if k == "comma":
        return _wrap(n.kids[0], P_COMMA) + [","] + \
            _wrap(n.kids[1], P_ASSIGN)
    raise RoundTripError("unparse: unknown node kind %r" % k)


def _sizeof_operand(text: str) -> List[str]:
    """`sizeof(T)` carries a type; `sizeof(expr)` carries an expression."""
    try:
        return canonical_type(text, as_cast=False)
    except NotATypeName:
        pass
    try:
        return unparse_tokens(parse_expression(text))
    except (ParseError, Ambiguous, RoundTripError) as e:
        raise NotATypeName("sizeof operand %r is neither type nor expression"
                           " (%s)" % (text, e))


def unparse(n: Node) -> str:
    """A spelling of `n` that re-tokenizes to exactly `unparse_tokens(n)`.

    Every token is separated by one space, so no two tokens can fuse (`-` `-`
    into `--`, `p` `->` into `p - > x`) and the round trip is well defined.
    """
    return " ".join(unparse_tokens(n))


def bare_tokens(text: str) -> List[str]:
    """Token spellings with whitespace, comments and parentheses removed."""
    return [t.text for t in tokenize(text)
            if t.kind != "eof" and t.text not in ("(", ")")]


# -------------------------------------------------------------------- check


@dataclass
class RoundTrip:
    ok: bool
    why: str = "round trip holds"
    canonical: str = ""
    failed_check: str = ""          # "type-name" | "sequence" | "idempotence"
    casts: int = 0

    def __bool__(self) -> bool:
        return self.ok


def check(text: str, known_ids=frozenset(), known_types=frozenset(),
          node: Optional[Node] = None) -> RoundTrip:
    """Validate that cexpr's tree for `text` really describes `text`.

    Returns a RoundTrip rather than raising, so a caller can report parser
    soundness SEPARATELY from the semantic verdict.  A failure here is never
    "this rewrite is unsafe" -- it is "the tool does not understand this
    source", which is a different and louder thing.
    """
    try:
        root = node if node is not None else \
            parse_expression(text, known_ids, known_types)
    except Ambiguous as e:
        return RoundTrip(False, "refused (ambiguous): %s" % e,
                         failed_check="parse")
    except ParseError as e:
        return RoundTrip(False, "refused (parse): %s" % e, failed_check="parse")

    casts = sum(1 for k in root.walk() if k.kind in ("cast", "sizeof"))

    # -- half 2: every cast type must be a type-name under a real grammar.
    try:
        toks = unparse_tokens(root)
    except NotATypeName as e:
        return RoundTrip(False, "MIS-PARSE: %s" % e, failed_check="type-name",
                         casts=casts)
    except RoundTripError as e:
        return RoundTrip(False, "unparse failed: %s" % e,
                         failed_check="unparse", casts=casts)

    canon = " ".join(toks)

    # -- half 1a: nothing dropped, duplicated or re-ordered.
    want = bare_tokens(text)
    got = [t for t in toks if t not in ("(", ")")]
    if want != got:
        if sorted(want) == sorted(got):
            why = "token ORDER differs: %r vs %r" % (" ".join(want),
                                                     " ".join(got))
        else:
            lost = _multiset_diff(want, got)
            gained = _multiset_diff(got, want)
            why = "token SET differs (lost %r, gained %r)" % (lost, gained)
        return RoundTrip(False, why, canon, "sequence", casts)

    # -- half 1b: the parser must agree with its own canonical spelling.
    try:
        again = parse_expression(canon, known_ids, known_types)
        canon2 = " ".join(unparse_tokens(again))
    except (ParseError, Ambiguous, RoundTripError) as e:
        return RoundTrip(False, "canonical form does not re-parse: %s" % e,
                         canon, "idempotence", casts)
    if canon2 != canon:
        return RoundTrip(False, "not idempotent: %r -> %r" % (canon, canon2),
                         canon, "idempotence", casts)

    return RoundTrip(True, "round trip holds", canon, "", casts)


def _multiset_diff(a: List[str], b: List[str]) -> List[str]:
    rest = list(b)
    out = []
    for x in a:
        if x in rest:
            rest.remove(x)
        else:
            out.append(x)
    return out


# --------------------------------------------------------------- self-test

# The two mutations this project has actually suffered, carried as controls
# so that no future edit can silence either one.
#
#   MUTATION A (A88, the parser bug): an ALL-CAPS object-like macro is
#   indistinguishable from a typedef to `_TYPEISH`, so the parser built a CAST
#   over a wrong-but-parsable tree.  It is reproduced here by NOT declaring
#   the macro -- exactly the state cexpr was in before project_macros().
#
#   MUTATION B (A87, the splice bug): `verts + j * 12` rewritten as
#   `(j + verts) * 12`.  The round trip's job on this one is to NOT erase the
#   difference: the added paren is load-bearing, so the two canonical forms
#   must differ.  A normaliser that dropped it would bless the mutation.

_IDS = frozenset({"obj", "fm", "p", "n", "sTab", "gMtx", "y", "x", "state",
                  "c", "count", "arr", "i", "cache", "idx", "verts", "j",
                  "a", "b", "d", "s", "m", "k", "f", "v", "t", "lo", "hi"})

_TYPES = frozenset({"Vec3f", "GameObject", "PlayerState", "Mtx"})

# (source, must_round_trip, note)
_ROUNDTRIP_CONTROLS: List[Tuple[str, bool, str]] = [
    # -- ordinary expressions
    ("verts + j * 12", True, "the A87 control's LEFT side"),
    ("(j + verts) * 12", True, "the A87 control's RIGHT side -- also valid C"),
    ("a * b + c * d", True, "two products in a sum"),
    ("a + (b + c)", True, "a load-bearing paren on the RIGHT of a +"),
    ("(a + b) + c", True, "a redundant paren on the LEFT of a +"),
    ("a - (b - c)", True, "load-bearing under subtraction"),
    ("p->pos.x * s", True, "member chain"),
    ("m[i][j] + k", True, "subscript chain"),
    ("f(a, b) + c", True, "call with two arguments"),
    ("c ? a : b", True, "conditional"),
    ("a ? b : c ? d : k", True, "right-associative conditional chain"),
    ("x = y = 0", True, "right-associative assignment chain"),
    ("(t = 1, x + y)", True, "comma beside the expression"),
    ("!a && b || c", True, "mixed logical precedence"),
    ("- -x", True, "two unary minuses must not fuse into `--`"),
    ("*p++", True, "postfix binds tighter than the dereference"),
    ("(*p)++", True, "and the paren that changes that is load-bearing"),
    ("a << b + c", True, "shift is looser than addition"),
    ("(a << b) + c", True, "redundant here, needed there"),
    ("~(a & 0xff)", True, "unary over a parenthesised mask"),
    ("i++ + n", True, "post-increment beside an add"),
    ("sizeof(Vec3f)", True, "sizeof over a type-name"),
    ("sizeof(a + b)", True, "sizeof over an expression"),
    # -- genuine casts: the type-name grammar must ACCEPT all of these
    ("(const Vec3f*)&obj->x", True, "qualifier before a typedef name"),
    ("(struct MldfNames*)sTab", True, "struct tag"),
    ("(const f32 (*)[4])fm", True, "pointer-to-array, qualified"),
    ("(u8(*)[2])((u8*)p + n)", True, "pointer-to-array, no space"),
    ("(f32 (*)[4])gMtx", True, "pointer-to-array"),
    ("(void (*)(f32, f32))p", True, "function-pointer type-name"),
    ("(unsigned long)x", True, "two combining core words"),
    ("(u8**)p", True, "pointer to pointer"),
    ("(f32)a * b", True, "a cast operand inside a product"),
    ("(GameObject*)obj", True, "plain typedef pointer"),
]

# (source, declare_the_macro, must_be_caught, note) -- MUTATION A.
#
# The mis-parse needs the exact shape `(NAME * (...)) <op> operand`: after the
# undeclared NAME, `_is_type_name` skips the `*` as a pointer, skips the
# parenthesised group as a function-pointer's parameter list, lands on `)` and
# says "type".  `(NAME - 1) * step` does NOT mis-parse -- the scan stops at the
# `-` -- so it is carried as a NEGATIVE control on the mutation itself, to keep
# the class description honest about how narrow the hole was.
_MACRO_CONTROLS: List[Tuple[str, bool, bool, str]] = [
    ("(SHIELD_SFX_VOLUME_MAX * (state->a / state->b)) + c", False, True,
     "MUTATION A undeclared: the parser builds a CAST -- must be CAUGHT"),
    ("(SHIELD_SFX_VOLUME_MAX * (state->a / state->b)) + c", True, False,
     "same source with the macro declared: a plain multiplication"),
    ("(GATE_SCALE * (arr[i] / n)) + b", False, True,
     "MUTATION A, second shape (subscript inside) -- must be CAUGHT"),
    ("(GATE_SCALE * (arr[i] / n)) + b", True, False, "same, declared"),
    ("(MAX_LOD_LEVELS - 1) * step", False, False,
     "NOT the hole: the type-name scan stops at `-`, so the tree is right"),
]

# Type slices that must be REJECTED by the independent grammar.  Each is a
# real expression that `_TYPEISH`-style shape matching would wave through.
_TYPE_REJECTS: List[Tuple[str, str]] = [
    ("SHIELD_SFX_VOLUME_MAX * (state->a / state->b)",
     "a product is not a type-name"),
    ("MAX_LOD_LEVELS - 1", "a difference is not a type-name"),
    ("Vec3f Mtx", "two typedef names in a row"),
    ("Vec3f + 1", "an addition is not a declarator"),
    ("const", "a qualifier with no type"),
    ("struct", "a tag keyword with no tag"),
    ("Vec3f (*)[", "unterminated array declarator"),
    ("Vec3f (x)", "a FUNCTION type -- legal type-name, illegal cast target"),
    ("Vec3f [4]", "an ARRAY type -- legal type-name, illegal cast target"),
    ("Vec3f * (state->a / state->b)",
     "the exact slice MUTATION A hands over, minus its outer parens"),
]

# ...and slices the grammar must ACCEPT under `sizeof`, where a function or
# array type IS a type-name.  This keeps the cast rule from being mistaken for
# a claim that these are ungrammatical.
_TYPE_SIZEOF_OK: List[Tuple[str, str]] = [
    ("Vec3f [4]", "an array type-name is fine under sizeof"),
    ("Vec3f (f32)", "a function type-name is fine under sizeof"),
]


def _self_test(verbose: bool = True) -> int:
    ok = fail = 0

    def say(good: bool, label: str, why: str) -> None:
        nonlocal ok, fail
        if good:
            ok += 1
            if verbose:
                print("  PASS  %-58s %s" % (label, why))
        else:
            fail += 1
            print("  FAIL  %-58s %s" % (label, why))

    kt = cexpr.project_typedefs() | _TYPES
    mac = cexpr.project_objects()

    print("--- ROUND TRIP: tree must describe the source it came from ---")
    for src, want, why in _ROUNDTRIP_CONTROLS:
        r = check(src, _IDS | mac, kt)
        say(bool(r) == want, src, why if bool(r) == want else r.why)

    print("--- TYPE-NAME GRAMMAR: these are NOT types ---")
    for src, why in _TYPE_REJECTS:
        try:
            got = canonical_type(src)
            say(False, "(%s)" % src, "accepted as %r -- %s" %
                (" ".join(got), why))
        except NotATypeName:
            say(True, "(%s)" % src, why)

    print("--- TYPE-NAME GRAMMAR: ...but these ARE, under sizeof ---")
    for src, why in _TYPE_SIZEOF_OK:
        try:
            canonical_type(src, as_cast=False)
            say(True, "sizeof(%s)" % src, why)
        except NotATypeName as e:
            say(False, "sizeof(%s)" % src, "rejected: %s" % e)

    print("--- MUTATION A (parser): an ALL-CAPS macro read as a cast ---")
    _MACROS = {"SHIELD_SFX_VOLUME_MAX", "GATE_SCALE", "MAX_LOD_LEVELS"}
    for src, declared, must_catch, why in _MACRO_CONTROLS:
        # Simulate the pre-fix parser exactly: the macro is simply not known.
        ids = _IDS | {"step"} | (_MACROS if declared else frozenset())
        r = check(src, ids, kt)
        if must_catch:
            good = (not r) and r.failed_check == "type-name"
            say(good, src, why if good else "NOT CAUGHT (%s / %s)" %
                (r.failed_check or "clean", r.why))
        else:
            say(bool(r), src, why if bool(r) else r.why)

    print("--- MUTATION B (splice): the load-bearing paren must SURVIVE ---")
    a = check("verts + j * 12", _IDS | mac, kt)
    b = check("(j + verts) * 12", _IDS | mac, kt)
    say(bool(a) and bool(b) and a.canonical != b.canonical,
        "verts + j * 12  vs  (j + verts) * 12",
        "canonical forms differ (%r vs %r)" % (a.canonical, b.canonical)
        if a.canonical != b.canonical else
        "NORMALISER ERASED THE DIFFERENCE -- the mutation would be blessed")
    # and the rewrites that ARE safe must normalise to something the prover
    # can still tell apart from the original text (round trip is not a
    # semantic verdict -- it must not pretend to be one).
    c = check("verts + 12 * j", _IDS | mac, kt)
    say(bool(c) and c.canonical != a.canonical,
        "verts + j * 12  vs  verts + 12 * j",
        "a legitimate commute also keeps a distinct canonical form")

    print("\n%d/%d controls hold" % (ok, ok + fail))
    return 1 if fail else 0


# ------------------------------------------------------------------- scan

def _scan(argv: List[str]) -> int:
    """Round-trip every expression region in every function in the tree.

    This is the measurement that says whether the check is usable as a gate.
    A mis-parse it reports is a real find; a legitimate expression it rejects
    is a false alarm that has to be fixed BEFORE it is wired into anything.
    The scan deliberately covers all 9498 functions, not just the sub-100 rows
    `expr_sweep` visits: a wrong tree in a 100%-matched function is still a
    wrong tree, and it is what a future rewrite there would be built on.
    """
    import json
    import expr_sweep
    from brute_match import REPO, find_function_body
    import semantic_equivalence as sem

    limit = None
    only_sub100 = "--sub100" in argv
    quiet = "--quiet" in argv
    # POSITIVE CONTROL on the scan itself.  `--no-macros` puts cexpr back in
    # the state A88 found it in -- object-like macros unknown -- so a clean
    # sweep with the fix in place can be told apart from a sweep that is
    # simply not looking at anything.  A scan that reports 0 both ways is
    # vacuous and must not be believed.
    no_macros = "--no-macros" in argv
    # ...and a sharper one.  `--no-macros` turns out to be a WEAK control on
    # the live tree (see the lane report): at the one real site the wrong tree
    # dies on a `,` and is reported as a refusal, not a mis-parse.  So the
    # non-vacuity proof cannot rest on it.  `--mutate` instead damages the
    # parser in a way that must light this scan up at scale:
    #   typeish -- every identifier is type-shaped, so `(x) * y` is a cast
    #   assoc   -- every binary operator has the same precedence, so the tree
    #              groups left-to-right regardless of what C says
    # If a mutated scan reports ~0 the scan is not looking at anything and no
    # clean result from it may be believed.
    mutate = ""
    for i, a in enumerate(argv):
        if a == "--mutate":
            mutate = argv[i + 1]
    if mutate in ("typeish", "typeish-silent"):
        cexpr._TYPEISH = re.compile(r"^\w+$")
        if mutate == "typeish-silent":
            # Also stop the parser refusing what it cannot decide.  Plain
            # `typeish` mostly converts damage into REFUSALS -- which is the
            # parser behaving well -- so it barely exercises the type-name
            # half.  Silencing the refusal is what turns the same damage into
            # the wrong-but-parsable trees this check exists for.
            cexpr.Parser._cast_is_ambiguous = lambda self, j: False
    elif mutate == "assoc":
        cexpr.BIN_PREC = {k: 12 for k in cexpr.BIN_PREC}
    elif mutate:
        print("unknown --mutate %r" % mutate)
        return 2
    for i, a in enumerate(argv):
        if a == "--limit":
            limit = int(argv[i + 1])

    cfg = json.loads((REPO / "build" / "GSAE01" / "config.json").read_text())
    # report unit names are the config name prefixed with the module dir and
    # stripped of `.c`; key both spellings so neither drifts.
    src_of: dict = {}
    for u in cfg["units"]:
        rel = u["name"].replace("\\", "/")
        src_of[rel[:-2] if rel.endswith(".c") else rel] = REPO / "src" / rel

    rep = json.loads((REPO / "build" / "GSAE01" / "report.json").read_text())
    rows: List[Tuple[str, str]] = []
    for u in rep["units"]:
        for f in (u.get("functions") or []):
            if only_sub100 and f.get("fuzzy_match_percent", 0.0) >= 100.0:
                continue
            rows.append((u["name"], f["name"]))
    if limit:
        rows = rows[:limit]

    kt = cexpr.project_typedefs()
    mac = frozenset() if no_macros else cexpr.project_objects()

    src_cache: dict = {}
    tot = clean = refused = bad = nofn = 0
    by_check: dict = {}
    seen_units = set()
    for unit, symbol in rows:
        path = src_of.get(unit) or src_of.get(unit.split("/", 1)[-1])
        if path is None or not path.exists():
            continue
        seen_units.add(unit)
        if unit not in src_cache:
            src_cache[unit] = path.read_bytes().decode("latin-1")
        src = src_cache[unit]
        body = find_function_body(src, symbol)
        if not body:
            nofn += 1
            continue
        dtext = expr_sweep.decl_text_of(src, body, symbol)
        oracle = sem.TypeOracle(dtext)
        ids = frozenset(oracle.kind) | mac | frozenset(
            re.findall(r"\b([a-z]\w*)\s*(?:=[^=]|\+\+|--|\[)", dtext))
        if mutate == "typeish-silent":
            # The declared-object set is what still protects `(count) * x`
            # even with `_TYPEISH` wide open, so the maximal-damage control
            # has to take that away too.
            ids = frozenset()
        for reg in expr_sweep.expression_regions(src, body[0], body[1]):
            text = src[reg.start:reg.end]
            if not text.strip():
                continue
            tot += 1
            try:
                r = check(text, ids, kt)
            except RecursionError:
                r = RoundTrip(False, "recursion", failed_check="unparse")
            if r.ok:
                clean += 1
            elif r.failed_check == "parse":
                refused += 1
            else:
                bad += 1
                by_check[r.failed_check] = by_check.get(r.failed_check, 0) + 1
                if not quiet:
                    print("  [%s] %s %s\n      %s\n      %s" %
                          (r.failed_check.upper(), unit, symbol,
                           " ".join(text.split())[:150], r.why[:220]))
    print("\n%d functions in %d units | scanned %d regions: %d round-trip "
          "clean, %d refused by the parser, %d MIS-PARSES %s (%d bodies not "
          "found)" % (len(rows), len(seen_units), tot, clean, refused, bad,
                      by_check or "", nofn))
    return 1 if bad else 0


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(_self_test())
    if "--scan" in sys.argv:
        sys.exit(_scan(sys.argv))
    print(__doc__)
