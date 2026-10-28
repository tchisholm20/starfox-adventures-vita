#!/usr/bin/env python3
"""A real C-expression tokenizer and parser, with source spans.

Every brute-force lane that rewrites source text has so far done it with
regular expressions over the raw characters.  A regex cannot see precedence, so
it cannot tell `verts + j * 12` (an offset added to a scaled index) from
`(j + verts) * 12` (a completely different computation) -- and because the
retail binary is compared BYTE-wise, a wrong computation whose bytes happen to
land closer scores as a win.  Nothing downstream can catch that: the score, the
object comparison and the forced link all measure agreement with retail's
bytes, never with retail's meaning.

This module is the parsing half of the fix.  semantic_equivalence.py is the
proving half.  Together they are the gate a rewrite must pass BEFORE it is
compiled, so that a rewrite which changes the arithmetic is rejected on its
meaning rather than rewarded for its score.

Nodes carry (start, end) offsets into the ORIGINAL text, so a caller can splice
a rewritten sub-expression back into a file while leaving every other byte --
including whitespace and comments -- exactly as it was.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import List, Optional, Sequence, Tuple

# --------------------------------------------------------------------- errors


class ParseError(Exception):
    """The text is not an expression this parser can read."""


class Ambiguous(ParseError):
    """`(x) * y` -- a cast or a multiply?  Undecidable without a symbol table."""


# ------------------------------------------------------------------ tokenizer

PUNCT = [
    "<<=", ">>=", "...",
    "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
    "(", ")", "[", "]", "{", "}", ".", ",", ";", "?", ":",
    "+", "-", "*", "/", "%", "&", "|", "^", "~", "!", "<", ">", "=",
]

_NUM = re.compile(
    r"(?:0[xX][0-9a-fA-F]+|(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?)"
    r"[uUlLfF]*")
_IDENT = re.compile(r"[A-Za-z_]\w*")


@dataclass
class Tok:
    kind: str          # id | num | str | chr | punct | eof
    text: str
    start: int
    end: int


def tokenize(s: str, lo: int = 0, hi: Optional[int] = None) -> List[Tok]:
    if hi is None:
        hi = len(s)
    out: List[Tok] = []
    i = lo
    while i < hi:
        c = s[i]
        if c in " \t\r\n\\":
            i += 1
            continue
        if s.startswith("//", i):
            j = s.find("\n", i)
            i = hi if j < 0 or j > hi else j
            continue
        if s.startswith("/*", i):
            j = s.find("*/", i)
            if j < 0:
                raise ParseError("unterminated comment")
            i = j + 2
            continue
        if c in "\"'":
            j = i + 1
            while j < hi:
                if s[j] == "\\":
                    j += 2
                    continue
                if s[j] == c:
                    j += 1
                    break
                j += 1
            else:
                raise ParseError("unterminated literal")
            out.append(Tok("str" if c == '"' else "chr", s[i:j], i, j))
            i = j
            continue
        m = _NUM.match(s, i)
        if m and (c.isdigit() or (c == "." and i + 1 < hi and s[i + 1].isdigit())):
            out.append(Tok("num", m.group(0), i, m.end()))
            i = m.end()
            continue
        m = _IDENT.match(s, i)
        if m:
            out.append(Tok("id", m.group(0), i, m.end()))
            i = m.end()
            continue
        for p in PUNCT:
            if s.startswith(p, i):
                out.append(Tok("punct", p, i, i + len(p)))
                i += len(p)
                break
        else:
            raise ParseError(f"stray character {c!r} at {i}")
    out.append(Tok("eof", "", hi, hi))
    return out


# ----------------------------------------------------------------------- AST


@dataclass
class Node:
    kind: str
    start: int = 0
    end: int = 0
    op: str = ""
    text: str = ""                       # id / num / str / chr / cast type
    kids: List["Node"] = field(default_factory=list)

    def src(self, s: str) -> str:
        return s[self.start:self.end]

    def walk(self):
        yield self
        for k in self.kids:
            yield from k.walk()


def _n(kind, start, end, op="", text="", kids=()) -> Node:
    return Node(kind, start, end, op, text, list(kids))


# -------------------------------------------------------------------- parser

BIN_PREC = {
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
ASSIGN_OPS = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="}

BASE_TYPE_WORDS = {
    "void", "char", "short", "int", "long", "float", "double", "signed",
    "unsigned", "const", "volatile", "struct", "union", "enum",
    "u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64", "f32", "f64",
    "bool", "BOOL", "size_t", "u128", "f80",
}
_TYPEISH = re.compile(r"^(?:[usf]\d+|.*_t|[A-Z]\w*)$")


class Parser:
    """Precedence-climbing parser for the C expression grammar.

    `known_ids` is the set of names the caller knows to be OBJECTS (locals,
    parameters, globals).  It is what disambiguates `(x) * y`: if x is a known
    object the parens are a grouping, if x is type-shaped it is a cast, and if
    neither can be established the parse is refused rather than guessed.
    """

    def __init__(self, s: str, toks: Sequence[Tok], known_ids=frozenset(),
                 known_types=frozenset()):
        self.s = s
        self.t = list(toks)
        self.i = 0
        self.known_ids = set(known_ids)
        self.known_types = set(known_types)

    # -- token helpers
    @property
    def cur(self) -> Tok:
        return self.t[self.i]

    def at(self, text: str) -> bool:
        c = self.cur
        return c.kind == "punct" and c.text == text

    def eat(self, text: str) -> Tok:
        if not self.at(text):
            raise ParseError(f"expected {text!r} got {self.cur.text!r}")
        tok = self.cur
        self.i += 1
        return tok

    # -- type-name recognition
    def _is_type_name(self, j: int) -> bool:
        """Do the tokens starting at j spell a type-name followed by `)`?"""
        k = j
        saw = False
        saw_name = False
        after_tag_kw = False
        while self.t[k].kind == "id":
            tx = self.t[k].text
            if tx in BASE_TYPE_WORDS:
                # `struct`/`union`/`enum` licenses the very next identifier as a
                # tag even when it is a known object name -- `(struct x*)` can
                # only ever be a type.
                after_tag_kw = tx in ("struct", "union", "enum")
            elif not saw_name and (
                    after_tag_kw or
                    (tx not in self.known_ids and
                     (tx in self.known_types or _TYPEISH.match(tx)))):
                # At most ONE typedef name or tag, but it may follow qualifiers
                # and `struct`/`union`/`enum`: `const Vec3f*`, `struct Mldf*`.
                saw_name = True
                after_tag_kw = False
            else:
                break
            saw = True
            k += 1
        if not saw:
            return False
        while self.t[k].kind == "punct" and self.t[k].text == "*":
            k += 1
        if self.t[k].kind == "punct" and self.t[k].text == "[":
            return False
        # a function-pointer type name: `void (*)(f32, f32)`, `int (*)(int)`,
        # or a pointer-to-array type name: `f32 (*)[4]`, `u8 (*)[2]`
        if self.t[k].kind == "punct" and self.t[k].text == "(":
            k = self._skip_group(k)
            if k < 0:
                return False
            if self.t[k].kind == "punct" and self.t[k].text == "(":
                k = self._skip_group(k)
                if k < 0:
                    return False
            else:
                while self.t[k].kind == "punct" and self.t[k].text == "[":
                    k = self._skip_brackets(k)
                    if k < 0:
                        return False
        return self.t[k].kind == "punct" and self.t[k].text == ")"

    def _skip_brackets(self, k: int) -> int:
        """Index just past the `]` matching the `[` at k, or -1."""
        depth = 0
        while k < len(self.t):
            tk = self.t[k]
            if tk.kind == "punct" and tk.text == "[":
                depth += 1
            elif tk.kind == "punct" and tk.text == "]":
                depth -= 1
                if depth == 0:
                    return k + 1
            elif tk.kind == "eof":
                return -1
            k += 1
        return -1

    def _skip_group(self, k: int) -> int:
        """Index just past the `)` matching the `(` at k, or -1."""
        depth = 0
        while k < len(self.t):
            tk = self.t[k]
            if tk.kind == "punct" and tk.text == "(":
                depth += 1
            elif tk.kind == "punct" and tk.text == ")":
                depth -= 1
                if depth == 0:
                    return k + 1
            elif tk.kind == "eof":
                return -1
            k += 1
        return -1

    def _cast_is_ambiguous(self, j: int) -> bool:
        """`(NAME)` where NAME is neither a known object nor type-shaped."""
        if self.t[j].kind != "id":
            return False
        if not (self.t[j + 1].kind == "punct" and self.t[j + 1].text == ")"):
            return False
        nm = self.t[j].text
        if nm in BASE_TYPE_WORDS or nm in self.known_types or nm in self.known_ids:
            return False
        if not _TYPEISH.match(nm):
            return False
        # type-shaped but never declared as either: only ambiguous when what
        # follows could begin a unary-expression.
        nxt = self.t[j + 2]
        return (nxt.kind in ("id", "num") or
                (nxt.kind == "punct" and nxt.text in ("(", "*", "&", "-", "~", "!", "+")))

    # -- grammar
    def parse(self) -> Node:
        n = self.expr()
        if self.cur.kind != "eof":
            raise ParseError(f"trailing {self.cur.text!r}")
        return n

    def expr(self) -> Node:
        n = self.assign()
        while self.at(","):
            self.i += 1
            r = self.assign()
            n = _n("comma", n.start, r.end, ",", kids=(n, r))
        return n

    def assign(self) -> Node:
        n = self.cond()
        c = self.cur
        if c.kind == "punct" and c.text in ASSIGN_OPS:
            self.i += 1
            r = self.assign()
            return _n("assign", n.start, r.end, c.text, kids=(n, r))
        return n

    def cond(self) -> Node:
        n = self.binary(4)
        if self.at("?"):
            self.i += 1
            a = self.expr()
            self.eat(":")
            b = self.cond()
            return _n("cond", n.start, b.end, "?:", kids=(n, a, b))
        return n

    def binary(self, min_prec: int) -> Node:
        n = self.unary()
        while True:
            c = self.cur
            if c.kind != "punct":
                return n
            p = BIN_PREC.get(c.text)
            if p is None or p < min_prec:
                return n
            self.i += 1
            r = self.binary(p + 1)
            n = _n("bin", n.start, r.end, c.text, kids=(n, r))

    def unary(self) -> Node:
        c = self.cur
        if c.kind == "punct" and c.text in ("+", "-", "!", "~", "*", "&", "++", "--"):
            self.i += 1
            k = self.unary()
            return _n("unary", c.start, k.end, c.text, kids=(k,))
        if c.kind == "id" and c.text == "sizeof":
            self.i += 1
            if self.at("("):
                op = self.eat("(")
                depth = 1
                j = self.i
                while depth:
                    tk = self.t[j]
                    if tk.kind == "eof":
                        raise ParseError("unterminated sizeof")
                    if tk.kind == "punct" and tk.text == "(":
                        depth += 1
                    elif tk.kind == "punct" and tk.text == ")":
                        depth -= 1
                    j += 1
                close = self.t[j - 1]
                self.i = j
                return _n("sizeof", c.start, close.end,
                          text=self.s[op.end:close.start].strip())
            k = self.unary()
            return _n("sizeof", c.start, k.end, kids=(k,))
        if c.kind == "punct" and c.text == "(":
            if self._cast_is_ambiguous(self.i + 1):
                raise Ambiguous(f"`({self.t[self.i + 1].text})` cast-or-group")
            if self._is_type_name(self.i + 1):
                # the type's closing paren is the one MATCHING the open paren,
                # not the first `)` in the stream: `(void (*)(f32, f32))` has
                # two nested groups before it.
                close = self._skip_group(self.i) - 1
                if close < 0:
                    raise ParseError("unterminated cast")
                ty = self.s[self.t[self.i + 1].start:self.t[close].start].strip()
                self.i = close + 1
                k = self.unary()
                return _n("cast", c.start, k.end, text=ty, kids=(k,))
        return self.postfix()

    def postfix(self) -> Node:
        n = self.primary()
        while True:
            c = self.cur
            if c.kind != "punct":
                return n
            if c.text == "(":
                self.i += 1
                args: List[Node] = []
                if not self.at(")"):
                    args.append(self.assign())
                    while self.at(","):
                        self.i += 1
                        args.append(self.assign())
                close = self.eat(")")
                n = _n("call", n.start, close.end, kids=[n] + args)
            elif c.text == "[":
                self.i += 1
                idx = self.expr()
                close = self.eat("]")
                n = _n("index", n.start, close.end, kids=(n, idx))
            elif c.text in (".", "->"):
                self.i += 1
                nm = self.cur
                if nm.kind != "id":
                    raise ParseError("member name expected")
                self.i += 1
                n = _n("member", n.start, nm.end, c.text, nm.text, kids=(n,))
            elif c.text in ("++", "--"):
                self.i += 1
                n = _n("postincr", n.start, c.end, c.text, kids=(n,))
            else:
                return n

    def primary(self) -> Node:
        c = self.cur
        if c.kind == "id":
            self.i += 1
            return _n("id", c.start, c.end, text=c.text)
        if c.kind in ("num", "str", "chr"):
            self.i += 1
            return _n(c.kind, c.start, c.end, text=c.text)
        if c.kind == "punct" and c.text == "(":
            self.i += 1
            inner = self.expr()
            close = self.eat(")")
            return _n("paren", c.start, close.end, kids=(inner,))
        raise ParseError(f"unexpected {c.text!r}")


_TYPEDEF_CACHE: Optional[frozenset] = None
_MACRO_CACHE: Optional[frozenset] = None
_ENUM_CACHE: Optional[frozenset] = None


def _walk_population(root):
    """The .h and source files a project-wide harvest should read.

    Sources come from the build's own answer (build.ninja plus group
    includes) when `root` is this repo and the tree is configured: a macro,
    enum or typedef spelled only in a source the build never compiles cannot
    shape a cast in live code.  A foreign root, or an unconfigured tree
    (no build.ninja yet), falls back to the filesystem walk.
    """
    import os
    import pathlib
    import sys
    root = pathlib.Path(root)
    files = []
    for sub in ("include", "src"):
        d = root / sub
        if d.is_dir():
            files += sorted(d.rglob("*.h"))
    here = pathlib.Path(__file__).resolve().parent
    if root.resolve() == here.parent and (root / "build.ninja").is_file():
        sys.path.insert(0, str(here))
        from source_coverage_audit import live_files_under
        files += [pathlib.Path(p) for p in
                  live_files_under("src", exts=(".c", ".cp", ".cpp"))]
    else:
        for sub in ("include", "src"):
            d = root / sub
            if d.is_dir():
                files += sorted(d.rglob("*.c"))
    return files


def project_macros(root=None) -> frozenset:
    """Every OBJECT-like macro name defined under include/ and src/.

    `_TYPEISH` calls anything capitalised type-shaped, so an ALL-CAPS macro
    constant is indistinguishable from a typedef name -- and that is not
    merely a refusal, it is a WRONG PARSE that succeeds:

        (SHIELD_SFX_VOLUME_MAX * (a / b)) + c

    reads as a CAST of `+c` to the type `SHIELD_SFX_VOLUME_MAX * (a / b)`
    rather than as a multiply.  Both spellings of a rewrite go through this
    same parser, so the semantic gate cannot catch it -- the only cure is to
    tell the parser these names are OBJECTS.  Function-like macros are
    excluded: `#define F(x)` is followed immediately by `(`, and a name used
    as `F(...)` parses as a call either way.
    """
    global _MACRO_CACHE
    if _MACRO_CACHE is not None:
        return _MACRO_CACHE
    import pathlib
    root = pathlib.Path(root or pathlib.Path(__file__).resolve().parent.parent)
    pat = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)(?![\w(])",
                     re.MULTILINE)
    names = set()
    for f in _walk_population(root):
        try:
            names |= set(pat.findall(
                f.read_bytes().decode("latin-1")))
        except OSError:
            continue
    # A name that is genuinely a typedef must stay a type even if some header
    # also #defines it; the type reading is the one that can appear in a cast.
    _MACRO_CACHE = frozenset(names - set(project_typedefs(root)))
    return _MACRO_CACHE


def project_enum_constants(root=None) -> frozenset:
    """Every enumeration constant declared under include/ and src/.

    The SAME hazard as `project_macros`, and the half of it that harvest
    missed: an enum constant is an ordinary capitalised identifier, so
    `_TYPEISH` calls it type-shaped exactly as it did an ALL-CAPS macro, and
    `(SOME_ENUM_CONST * (a / b)) + c` builds the same wrong-but-parsable cast.
    2354 of them are declared under include/ and src/ and every one was known
    to this parser as neither object nor type.  Found by
    `tools/cexpr_roundtrip.py`, which catches the wrong tree whether or not
    this list is complete -- the list only stops the tree being built.
    """
    global _ENUM_CACHE
    if _ENUM_CACHE is not None:
        return _ENUM_CACHE
    import pathlib
    root = pathlib.Path(root or pathlib.Path(__file__).resolve().parent.parent)
    body = re.compile(r"\benum\b[^{;]*\{([^}]*)\}", re.S)
    names = set()
    for f in _walk_population(root):
        try:
            txt = f.read_bytes().decode("latin-1")
        except OSError:
            continue
        for m in body.finditer(txt):
            inner = re.sub(r"/\*.*?\*/", " ", m.group(1), flags=re.S)
            inner = re.sub(r"//[^\n]*", " ", inner)
            for part in inner.split(","):
                mm = re.match(r"\s*([A-Za-z_]\w*)\s*(?:=|$)",
                              part.rstrip())
                if mm:
                    names.add(mm.group(1))
    _ENUM_CACHE = frozenset(names - set(project_typedefs(root)))
    return _ENUM_CACHE


def project_objects(root=None) -> frozenset:
    """Every project-wide name the parser must treat as an OBJECT, not a type.

    Object-like macros plus enumeration constants.  Callers that used to pass
    `project_macros()` should pass this: a name is a cast hazard because it is
    capitalised, and nothing about being a macro rather than an enum constant
    makes it more or less so.
    """
    return project_macros(root) | project_enum_constants(root)


def project_typedefs(root=None) -> frozenset:
    """Every typedef name declared under include/ and src/.

    `(MtxPtr)x` is a cast and `(count)*x` is a multiply, and nothing in the
    token stream distinguishes them -- so without the real typedef list the
    parser has to refuse both.  That refusal was the single largest cause of
    skipped expressions in the first sweep (48 of 65), and a region a tool
    cannot parse looks exactly like a region it examined and cleared.
    """
    global _TYPEDEF_CACHE
    if _TYPEDEF_CACHE is not None:
        return _TYPEDEF_CACHE
    import pathlib
    root = pathlib.Path(root or pathlib.Path(__file__).resolve().parent.parent)
    names = set()
    pat = re.compile(r"\btypedef\b")
    fnptr = re.compile(r"\(\s*\*+\s*([A-Za-z_]\w*)\s*\)")
    for f in _walk_population(root):
        names |= _typedefs_in(f, pat, fnptr)
    _TYPEDEF_CACHE = frozenset(names)
    return _TYPEDEF_CACHE


def _typedefs_in(path, pat, fnptr) -> set:
    try:
        txt = path.read_text("latin-1")
    except OSError:
        return set()
    out = set()
    for m in pat.finditer(txt):
        i, depth = m.end(), 0
        while i < len(txt):
            c = txt[i]
            if c in "{([":
                depth += 1
            elif c in "})]":
                depth -= 1
            elif c == ";" and depth <= 0:
                break
            i += 1
        decl = txt[m.end():i]
        fp = fnptr.search(decl)
        if fp:
            out.add(fp.group(1))
            continue
        # `typedef struct {...} A, *B;` -- every declarator after the body
        tail = decl[decl.rfind("}") + 1:] if "}" in decl else decl
        for part in tail.split(","):
            ids = _IDENT.findall(part)
            if ids:
                out.add(ids[-1])
    return out


def parse_expression(text: str, known_ids=frozenset(),
                     known_types=frozenset(), lo: int = 0,
                     hi: Optional[int] = None) -> Node:
    toks = tokenize(text, lo, hi)
    return Parser(text, toks, known_ids, known_types).parse()


# ------------------------------------------------------------------- helpers


def strip_parens(n: Node) -> Node:
    while n.kind == "paren":
        n = n.kids[0]
    return n


IMPURE_KINDS = {"call", "assign", "postincr", "comma"}


def is_pure(n: Node) -> bool:
    """No call, no assignment, no ++/--, no comma: reordering is observable
    only through those, so a pure node may be evaluated in any order."""
    for k in n.walk():
        if k.kind in IMPURE_KINDS:
            return False
        if k.kind == "unary" and k.op in ("++", "--"):
            return False
    return True


def normal_text(n: Node, s: str) -> str:
    """Whitespace/comment-free spelling, used to key identical leaves."""
    return "".join(t.text for t in tokenize(n.src(s)) if t.kind != "eof")


# ------------------------------------------------------------- self-test
# The type-name grammar is the parser's sharpest edge: every widening makes
# more expressions reachable, and every widening risks reading an ordinary
# parenthesised expression as a cast.  A wrong tree here is INVISIBLE to
# tools/semantic_equivalence.py, because both spellings of a rewrite are
# parsed by THIS code -- a systematic mis-parse agrees with itself.  So the
# negative controls below matter more than the positive ones.

_SELFTEST_IDS = frozenset({
    "obj", "fm", "p", "n", "sTab", "gMtx", "y", "x", "state", "c",
    "count", "arr", "i", "cache", "idx",
})


def _self_test() -> int:
    kt = project_typedefs()
    mac = project_objects()
    ids = _SELFTEST_IDS | mac
    ok = fail = 0

    def check(src, want_kind, why):
        nonlocal ok, fail
        try:
            got = parse_expression(src, ids, kt, 0, len(src)).kind
        except (ParseError, Ambiguous) as e:
            got = "%s: %s" % (type(e).__name__, e)
        if got == want_kind:
            ok += 1
            print("  PASS  %-52s %s" % (src, why))
        else:
            fail += 1
            print("  FAIL  %-52s want %s got %s" % (src, want_kind, got))

    print("--- POSITIVE: type-names that must parse as casts ---")
    check("(const Vec3f*)&obj->x", "cast", "qualifier before a typedef name")
    check("(struct MldfNames*)sTab", "cast", "struct tag")
    check("(const f32 (*)[4])fm", "cast", "pointer-to-array, qualified")
    check("(u8(*)[2])((u8*)p + n)", "cast", "pointer-to-array, no space")
    check("(f32 (*)[4])gMtx", "cast", "pointer-to-array")

    print("--- NEGATIVE: expressions that must NOT become casts ---")
    check("(count) * x", "bin", "a known object is not a type")
    check("(count) - x", "bin", "a known object is not a type")
    check("(arr)[i]", "index", "a subscript is not a cast")
    check("(p) + 1", "bin", "a known object is not a type")
    # The macro hazard: _TYPEISH calls anything capitalised type-shaped, so
    # without project_macros() this reads as a CAST of `+ c` to the type
    # `SHIELD_SFX_VOLUME_MAX * (state->a / state->b)` -- silently, and with a
    # completely wrong tree that still parses.
    check("(SHIELD_SFX_VOLUME_MAX * (state->a / state->b)) + c", "bin",
          "an object-like macro is an OBJECT, not a type")
    check("(SHIELD_SFX_VOLUME_MAX * (state->a / state->b))", "paren",
          "same, standalone")
    # The other half of the same hazard, missed by the macro harvest and found
    # by tools/cexpr_roundtrip.py: an ENUM CONSTANT is just as capitalised as
    # an ALL-CAPS macro, and 2354 of them were known to this parser as neither
    # object nor type.  `project_objects()` covers both.
    _enum = next(iter(sorted(project_enum_constants())), None)
    if _enum:
        check("(%s * (state->a / state->b)) + c" % _enum, "bin",
              "an enum constant is an OBJECT, not a type")

    print("\n%d/%d controls hold" % (ok, ok + fail))
    return 1 if fail else 0


if __name__ == "__main__":
    import sys as _sys
    if "--self-test" in _sys.argv:
        _sys.exit(_self_test())
    print("cexpr: a C-expression parser. Run --self-test for the controls.")
