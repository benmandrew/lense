"""Turn lense's JSON report into C++ that checks every claim at runtime.

The checker is the generated program plus a main() that, for each record:

- compares sizeof and alignof;
- builds an object in zeroed storage and compares the address of every base
  subobject and field with the reported offset;
- assigns -1 to every named bitfield and checks which bits changed;
- reads every vtable pointer and checks that the table it points at belongs
  to the complete object (its RTTI) and to the vptr's subobject (its
  offset-to-top).

batch_checker() puts several of these checks into one program, each case in its
own namespace, so that one build serves many cases. macOS checks every new
executable before its first run, one at a time, so building fewer of them is
what makes fuzzing scale with cores there.

Both also check the record's traits (standard layout, trivially copyable,
unique object representations) and the alignment of its direct named fields.
Given an Extra, they also build each `reorder` suggestion: a copy of the
program, in its own namespace, where only that record's fields are permuted,
and check that clang gives the copy the size lense promised.

A second, static checker uses static_assert only, for targets that cannot run
here (x86-64 Linux, Microsoft). Their standard library headers are not
available, so it uses the compiler builtins that <type_traits> is built on.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

from props import is_field

PRELUDE = r"""
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <type_traits>
#include <typeinfo>

static int lense_failures = 0;
static void lense_check(const char *what, long long got, long long want) {
  if (got != want) {
    std::printf("FAIL %s: got %lld, want %lld\n", what, got, want);
    ++lense_failures;
  }
}
// Finds the changed bits between two snapshots of an object.
static void lense_diff(const unsigned char *a, const unsigned char *b,
                       std::size_t n, long long *first, long long *count,
                       long long *span) {
  long long lo = -1, hi = -1, c = 0;
  for (std::size_t i = 0; i < n * 8; ++i) {
    if (((a[i / 8] ^ b[i / 8]) >> (i % 8)) & 1) {
      if (lo < 0) lo = (long long)i;
      hi = (long long)i;
      ++c;
    }
  }
  *first = lo;
  *count = c;
  *span = lo < 0 ? 0 : hi - lo + 1;
}
static long long lense_max(long long a, long long b) { return a > b ? a : b; }
"""

# Failure lines of reorder checks start with this, so the driver can tell
# them apart from layout mismatches.
REORDER = "reorder of "

# (JSON key, std trait, the builtin it is built on)
TRAITS = [
    ("standard_layout", "std::is_standard_layout_v", "__is_standard_layout"),
    ("trivially_copyable", "std::is_trivially_copyable_v",
     "__is_trivially_copyable"),
    ("unique_object_representations",
     "std::has_unique_object_representations_v",
     "__has_unique_object_representations"),
]


@dataclass
class Extra:
    """What the checkers need beyond the report, from the generator's model.

    aligns:   record -> field name -> its alignas (0 if none).
    reorders: (record, program with that record's fields in lense's
              suggested order, the size lense gave that order).
    """
    aligns: Dict[str, Dict[str, int]] = field(default_factory=dict)
    reorders: List[Tuple[str, str, int]] = field(default_factory=list)


def reorder_ns(name: str) -> str:
    return f"lense_reorder_{name}"


def reorders(extra: Optional[Extra]) -> List[Tuple[str, str, int]]:
    return extra.reorders if extra else []


def reorder_decls(extra: Optional[Extra]) -> List[str]:
    out = []
    for name, program, _ in reorders(extra):
        out += [f"namespace {reorder_ns(name)} {{", program,
                f"}}  // namespace {reorder_ns(name)}"]
    return out


def direct_fields(r: dict, extra: Optional[Extra]):
    """(name, alignas, reported align) of r's direct named non-bitfields."""
    aligns = extra.aligns.get(r["name"]) if extra else None
    if aligns is None:
        return
    for n in r["members"]:
        if is_field(n) and n.get("name") in aligns and "align" in n:
            yield n["name"], aligns[n["name"]], n["align"]


def cstr(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


class Checker:
    def __init__(self, report: dict, extra: Optional[Extra] = None):
        self.extra = extra
        self.records: Dict[str, dict] = {r["name"]: r for r in report["records"]}
        self.ptr = report["pointer_size"]
        self.lines: List[str] = []

    def emit(self, s: str) -> None:
        self.lines.append("  " + s)

    def check(self, what: str, got: str, want) -> None:
        self.emit(f"lense_check({cstr(what)}, (long long)({got}), {want}LL);")

    def check_expr(self, expr: str, want) -> None:
        self.check(expr, expr, want)

    def offset_of(self, expr: str) -> str:
        return f"reinterpret_cast<const char *>(&({expr})) - b"

    def record(self, r: dict) -> None:
        name = r["name"]
        buf = f"buf_{name}"
        self.lines.append(f"  {{ // {name}")
        self.check_expr(f"sizeof({name})", r["size"])
        self.check_expr(f"alignof({name})", r["align"])
        for key, trait, _ in TRAITS if "traits" in r else []:
            self.check_expr(f"{trait}<{name}>", int(r["traits"][key]))
        for f, alignas, want in direct_fields(r, self.extra):
            self.check(f"align of {name}.{f}",
                       f"lense_max(alignof(decltype({name}::{f})), {alignas})", want)
        self.emit(f"alignas({name}) static unsigned char {buf}[sizeof({name})];")
        self.emit(f"std::memset({buf}, 0, sizeof {buf});")
        self.emit(f"{name} *p = ::new (static_cast<void *>({buf})) {name};")
        self.emit("const char *b = reinterpret_cast<const char *>(p);")
        self.emit("(void)b;")
        ctx = (0, name)
        for m in r["members"]:
            self.node(m, "(*p)", name, ctx, buf)
        self.lines.append("  }")

    def node(self, n: dict, parent: str, label: str, ctx, buf: str) -> None:
        kind = n["kind"]
        off = n["offset"]
        if kind == "record":
            role = n["role"]
            if role == "field":
                expr = f"({parent}).{n['name']}"
                where = f"{label}.{n['name']}"
                child_ctx = (off, n["type"])
            else:
                expr = f"static_cast<{n['type']} &>({parent})"
                where = f"{label}::{n['type']}"
                child_ctx = ctx
            self.check(f"offset of {where}", self.offset_of(expr), off)
            for c in n["members"]:
                self.node(c, expr, where, child_ctx, buf)
        elif kind == "field":
            where = f"{label}.{n['name']}"
            self.check(f"offset of {where}",
                       self.offset_of(f"({parent}).{n['name']}"), off)
        elif kind == "bitfield" and "name" in n:
            where = f"{label}.{n['name']}"
            self.emit("{")
            self.emit(f"  unsigned char before[sizeof {buf}];")
            self.emit(f"  std::memcpy(before, {buf}, sizeof before);")
            self.emit(f"  ({parent}).{n['name']} = -1;")
            self.emit("  long long first, count, span;")
            self.emit(f"  lense_diff(before, {buf}, sizeof before, &first, "
                      "&count, &span);")
            self.emit(f"  std::memcpy({buf}, before, sizeof before);")
            self.emit(f"  lense_check({cstr('bit offset of ' + where)}, first, "
                      f"{n['bit_offset']}LL);")
            self.emit(f"  lense_check({cstr('bit width of ' + where)}, count, "
                      f"{n['bit_size']}LL);")
            self.emit(f"  lense_check({cstr('contiguous bits of ' + where)}, "
                      "span, count);")
            self.emit("}")
        elif kind == "vptr":
            self.vptr(n, label, ctx)

    def vptr(self, n: dict, label: str, ctx) -> None:
        # Itanium: a vptr sits at the start of its subobject and points just
        # past [offset_to_top, RTTI] of a table in the complete object's group.
        coff, complete = ctx
        if complete not in self.records:
            return
        where = f"vptr of {n['type']} at {label}+{n['offset']}"
        self.emit("{")
        self.emit(f"  const void *const *V = *reinterpret_cast<const void *const "
                  f"*const *>(b + {n['offset']});")
        self.emit(f"  lense_check({cstr(where + ': RTTI')}, V[-1] == "
                  f"static_cast<const void *>(&typeid({complete})), 1LL);")
        self.emit(f"  lense_check({cstr(where + ': offset_to_top')}, (long long)"
                  f"reinterpret_cast<const std::ptrdiff_t *>(V)[-2], "
                  f"{coff - n['offset']}LL);")
        self.emit("}")

    def body(self) -> List[str]:
        body = []
        for r in self.records.values():
            self.lines = []
            self.record(r)
            body.extend(self.lines)
        for name, _, size in reorders(self.extra):
            body.append(f"  lense_check({cstr(REORDER + name + ': sizeof')}, "
                        f"(long long)sizeof({reorder_ns(name)}::{name}), {size}LL);")
        return body

    def source(self, program: str) -> str:
        return (PRELUDE + "\n" + program + "\n" +
                "\n".join(reorder_decls(self.extra)) + "\n\nint main() {\n" +
                "\n".join(self.body()) +
                '\n  if (lense_failures) std::printf("%d failures\\n", '
                "lense_failures);\n  return lense_failures ? 1 : 0;\n}\n")


def runtime_checker(program: str, report: dict,
                    extra: Optional[Extra] = None) -> str:
    return Checker(report, extra).source(program)


BEGIN = "@@lense begin "
END = "@@lense end "


def batch_checker(cases: List[Tuple[str, dict, Optional[Extra]]]) -> str:
    """One program that runs the runtime checks of several cases.

    Case i lives in namespace lense_case<i>. main() prints BEGIN i before it
    and END i <failures> after it, unbuffered, so that the driver can tell
    which case was running if the program crashes.
    """
    out = [PRELUDE]
    for i, (program, report, extra) in enumerate(cases):
        out += [f"namespace lense_case{i} {{", program, *reorder_decls(extra),
                "static void run() {", *Checker(report, extra).body(), "}",
                f"}}  // namespace lense_case{i}", ""]
    out += ["int main() {", "  std::setvbuf(stdout, nullptr, _IONBF, 0);"]
    for i in range(len(cases)):
        out.append(f'  {{ int before = lense_failures; std::printf("{BEGIN}{i}\\n"); '
                   f'lense_case{i}::run(); std::printf("{END}{i} %d\\n", '
                   "lense_failures - before); }")
    out += ["  return 0;", "}"]
    return "\n".join(out) + "\n"


def static_checker(program: str, report: dict,
                   extra: Optional[Extra] = None) -> str:
    """static_asserts on sizes, alignment, traits and offsetof for direct
    members, and on the size of every reorder suggestion."""
    out = [program, ""] + reorder_decls(extra) + [""]

    def walk(path: str, members: List[dict], root: str):
        for n in members:
            if is_field(n):
                p = f"{path}.{n['name']}" if path else n["name"]
                out.append(f"static_assert(__builtin_offsetof({root}, {p}) == "
                           f"{n['offset']}, {cstr('offset of ' + root + '.' + p)});")
                if n["kind"] == "record":
                    walk(p, n["members"], root)

    for r in report["records"]:
        name = r["name"]
        out.append(f"static_assert(sizeof({name}) == {r['size']}, "
                   f"{cstr('sizeof ' + name)});")
        out.append(f"static_assert(alignof({name}) == {r['align']}, "
                   f"{cstr('alignof ' + name)});")
        for key, _, builtin in TRAITS if "traits" in r else []:
            want = "true" if r["traits"][key] else "false"
            out.append(f"static_assert({builtin}({name}) == {want}, "
                       f"{cstr(key + ' ' + name)});")
        for f, alignas, want in direct_fields(r, extra):
            a = f"alignof(decltype({name}::{f}))"
            out.append(f"static_assert(({a} > {alignas} ? {a} : {alignas}) == "
                       f"{want}, {cstr('align of ' + name + '.' + f)});")
        walk("", r["members"], name)
    for name, _, size in reorders(extra):
        out.append(f"static_assert(sizeof({reorder_ns(name)}::{name}) == {size}, "
                   f"{cstr(REORDER + name + ': sizeof')});")
    return "\n".join(out) + "\n"
