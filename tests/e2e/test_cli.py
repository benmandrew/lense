#!/usr/bin/env python3
"""End-to-end tests of the lense command line.

Each test runs the real binary (path in $LENSE) as a user would: files on
disk, standard input, options, pass-through clang arguments, exit codes.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest

LENSE = os.environ.get("LENSE", "lense")
LINUX = "--target=x86_64-linux-gnu"
MSVC = "--target=x86_64-pc-windows-msvc"

DERIVED = """\
struct Base { virtual ~Base(); virtual void f(); int a; };
struct Other { virtual void g(); char c; };
struct Derived : Base, Other { void f() override; double d; char e; };
"""


def lense(*args: str, stdin: str | None = None, cwd: str | None = None):
    return subprocess.run([LENSE, *args], input=stdin, capture_output=True,
                          text=True, cwd=cwd, timeout=120)


def records(report: str | dict) -> dict:
    """A JSON report, as text or parsed, keyed by record name."""
    if isinstance(report, str):
        report = json.loads(report)
    return {r["name"]: r for r in report["records"]}


def linux_json(*args: str, stdin: str | None = None) -> dict:
    return records(lense("--json", LINUX, *args, stdin=stdin).stdout)


def linux_text(*args: str, stdin: str | None = None) -> str:
    return lense("--color=false", LINUX, *args, stdin=stdin).stdout


def named(rec: dict) -> dict:
    """Every named member of a record, at any depth, by name."""
    out = {}

    def walk(ms):
        for m in ms:
            if m.get("name"):
                out[m["name"]] = m
            if m["kind"] == "record":
                walk(m["members"])
    walk(rec["members"])
    return out


def vptr_offsets(members: list) -> list:
    out = []
    for n in members:
        if n["kind"] == "vptr":
            out.append(n["offset"])
        out.extend(vptr_offsets(n.get("members", [])))
    return out


class Cli(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = tmp.name

    def write(self, name: str, text: str) -> str:
        path = os.path.join(self.dir, name)
        with open(path, "w") as f:
            f.write(text)
        return path

    # -- basics -------------------------------------------------------------

    def test_help_lists_options(self):
        r = lense("--help")
        self.assertEqual(r.returncode, 0)
        for opt in ("--json", "--target", "--record", "--expand-system", "--source", "--lang", "--color",
                    "--compare", "--cache-line"):
            self.assertIn(opt, r.stdout)

    def test_version(self):
        r = lense("--version")
        self.assertEqual(r.returncode, 0)
        self.assertIn("LLVM 23", r.stdout)

    def test_missing_file_is_usage_error(self):
        r = lense(os.path.join(self.dir, "nope.cpp"))
        self.assertEqual(r.returncode, 2)
        self.assertIn("cannot read", r.stderr)

    def test_bad_lang_is_usage_error(self):
        self.assertEqual(lense("--lang=rust", "-", stdin="").returncode, 2)

    def test_file_input(self):
        r = lense("--json", LINUX, self.write("t.cpp", DERIVED))
        self.assertEqual(r.returncode, 0, r.stderr)
        recs = records(r.stdout)
        self.assertEqual(set(recs), {"Base", "Other", "Derived"})
        self.assertEqual(recs["Derived"]["size"], 48)
        self.assertEqual(recs["Derived"]["location"], {"line": 3, "column": 8})

    def test_stdin_cpp_and_c(self):
        r = lense("--json", LINUX, "-", stdin="struct S { bool b; int x; };")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(records(r.stdout)["S"]["size"], 8)
        # `class` is an identifier in C and a keyword in C++.
        src = "struct S { int class; };"
        self.assertEqual(lense("--json", LINUX, "--lang=c", "-", stdin=src).returncode, 0)
        self.assertEqual(lense("--json", LINUX, "-", stdin=src).returncode, 1)

    def test_c_file_by_extension(self):
        r = lense("--json", LINUX, self.write("t.c", "struct s { char c; double d; };\n"))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(records(r.stdout)["s"]["padding_bits"], 7 * 8)

    # -- options ------------------------------------------------------------

    def test_record_filter(self):
        path = self.write("t.cpp", DERIVED)
        self.assertEqual(set(linux_json("--record=Derived,Other", path)), {"Derived", "Other"})
        r = lense("--json", LINUX, "--record=Nothing", path)
        self.assertEqual(r.returncode, 0)
        self.assertEqual(records(r.stdout), {})

    def test_qualified_record_name(self):
        recs = linux_json("--record=a::b::S", "-",
                          stdin="namespace a { namespace b { struct S { int x; }; } }")
        self.assertEqual(set(recs), {"a::b::S"})

    def test_vptrs_without_vtables(self):
        path = self.write("t.cpp", DERIVED)
        d = linux_json(path)["Derived"]
        self.assertNotIn("vtables", d)
        self.assertNotIn("points_to", json.dumps(d))
        self.assertEqual(vptr_offsets(d["members"]), [0, 16])
        text = linux_text(path)
        self.assertNotIn("vtable for", text)
        self.assertIn("vptr", text)
        self.assertNotEqual(lense("--vtables", path).returncode, 0)

    def test_source_above_each_record(self):
        path = self.write("t.cpp", "// Two records.\n"
                          "struct A {\n  int a;\n};\nstruct B { char b; };\n")
        text = linux_text("--source", path)
        self.assertTrue(text.startswith(
            "   2 | struct A {\n   3 |   int a;\n   4 | };\nstruct A  size 4"), text)
        self.assertIn("\n   5 | struct B { char b; };\nstruct B  size 1", text)
        self.assertNotIn(" | ", linux_text(path))

    def test_source_highlighting(self):
        path = self.write("t.cpp", "struct A {\n  /* two\n     lines */\n"
                          "  int a = 7; // seven\n};\n")
        text = lense("--color", LINUX, "--source", path).stdout
        blue, green, dim, reset = "\x1b[0;34m", "\x1b[0;32m", "\x1b[0;90m", "\x1b[0m"
        self.assertIn(f"{blue}struct{reset} A {{", text)
        self.assertIn(f"{blue}int{reset} a = {green}7{reset}; {dim}// seven{reset}", text)
        # A block comment stays dim on both of its lines.
        self.assertIn(f"{dim}/* two{reset}", text)
        self.assertIn(f"{dim}     lines */{reset}", text)

    def test_source_in_json_only_when_asked(self):
        path = self.write("t.cpp", "\nstruct A { int a; };\n")
        self.assertEqual(linux_json("--source", path)["A"]["source"],
                         {"line": 2, "text": "struct A { int a; };\n"})
        self.assertNotIn("source", linux_json(path)["A"])

    def test_source_shapes(self):
        path = self.write("t.cpp", """\
template <typename T>
struct Box { T v; };
Box<int> b;
typedef struct { short s; } Anon;
void f() {
    struct Local {
        int y;
    } l;
}
""")
        recs = linux_json("--source", path)
        # An instantiation shows its template.
        self.assertEqual(recs["Box<int>"]["source"],
                         {"line": 1, "text": "template <typename T>\nstruct Box { T v; };\n"})
        # An anonymous struct shows the typedef that names it.
        self.assertEqual(recs["Anon"]["source"]["text"],
                         "typedef struct { short s; } Anon;\n")
        # A local class loses the indentation of the function body.
        self.assertEqual(recs["f()::Local"]["source"]["text"],
                         "struct Local {\n    int y;\n}\n")

    def test_target_switches_abi(self):
        path = self.write("t.cpp", DERIVED)
        itanium = json.loads(lense("--json", LINUX, path).stdout)
        msvc = json.loads(lense("--json", MSVC, path).stdout)
        self.assertEqual(itanium["abi"], "itanium")
        self.assertEqual(itanium["target"], "x86_64-unknown-linux-gnu")
        self.assertEqual(msvc["abi"], "microsoft")
        self.assertEqual(vptr_offsets(records(msvc)["Derived"]["members"]), [0, 16])
        self.assertEqual(json.loads(lense("--json", "--target=i686-linux-gnu", path).stdout)
                         ["pointer_size"], 4)

    def test_clang_arguments_pass_through(self):
        path = self.write("t.cpp", "struct S { char buf[WIDTH]; };\n")
        r = lense("--json", LINUX, path, "--", "-DWIDTH=13")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(records(r.stdout)["S"]["size"], 13)
        self.assertEqual(lense("--json", LINUX, path).returncode, 1)

    def test_quote_include_relative_to_file(self):
        self.write("inner.h", "struct Inner { char c; int i; };\n")
        path = self.write("main.cpp", '#include "inner.h"\nstruct Outer { Inner in; char tail; };\n')
        r = lense("--json", LINUX, path, cwd="/")
        self.assertEqual(r.returncode, 0, r.stderr)
        recs = records(r.stdout)
        # Only records from the main file are listed, but Inner is expanded.
        self.assertEqual(set(recs), {"Outer"})
        inner = recs["Outer"]["members"][0]
        self.assertEqual([m["name"] for m in inner["members"] if "name" in m], ["c", "i"])

    def test_header_function_bodies_skipped(self):
        # The error in broken() is never parsed. Bodies that a layout
        # depends on (constexpr, deduced return type) still are.
        self.write("h.h", (
            "constexpr int n() { int k = 0; for (int i = 0; i < 5; ++i) k += i; return k; }\n"
            "template <class T> constexpr int g() { return sizeof(T) * 3; }\n"
            "inline auto f() { struct L { int x; double y; }; return L{}; }\n"
            "inline int broken() { return undeclared; }\n"))
        path = self.write("main.cpp", (
            '#include "h.h"\n'
            "struct S { int a[n()]; char c[g<long>()]; decltype(f()) m; };\n"))
        r = lense("--json", LINUX, path)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(records(r.stdout)["S"]["size"], 80)

    def test_main_file_function_bodies_parsed(self):
        src = "void f() { struct Local { int a; }; }\nint g() { return undeclared; }\n"
        r = lense("--json", LINUX, "-", stdin=src)
        self.assertEqual(r.returncode, 1)
        self.assertIn("undeclared", r.stderr)
        self.assertEqual(set(records(r.stdout)), {"f()::Local"})

    def test_system_types_collapse_unless_expanded(self):
        src = "#include <string>\nstruct S { std::string s; bool b; };\n"
        collapsed = lense("--json", "-", stdin=src)
        self.assertEqual(collapsed.returncode, 0, collapsed.stderr)
        self.assertEqual(records(collapsed.stdout)["S"]["members"][0]["kind"], "field")
        expanded = records(lense("--json", "--expand-system", "-", stdin=src).stdout)
        self.assertEqual(expanded["S"]["members"][0]["kind"], "record")

    def test_color(self):
        path = self.write("t.cpp", DERIVED)
        self.assertIn("\x1b[", lense("--color", LINUX, path).stdout)
        self.assertNotIn("\x1b[", linux_text(path))
        # Output to a pipe is not a terminal, so auto means no colour.
        self.assertNotIn("\x1b[", lense(LINUX, path).stdout)

    # -- layout notes --------------------------------------------------------

    def test_member_alignment(self):
        src = "struct S { char c; alignas(8) int i; double d[2]; };\n"
        m = named(linux_json("-", stdin=src)["S"])
        self.assertEqual((m["c"]["align"], m["i"]["align"], m["d"]["align"]), (1, 8, 8))

    def test_align_source(self):
        src = ("struct M { char c; double d; };\n"
               "struct alignas(16) A { char c; };\n"
               "struct __attribute__((packed)) P { int i; };\n"
               "#pragma pack(2)\nstruct K { double d; };\n#pragma pack()\n"
               "struct V { virtual void f(); };\n"
               "struct D : M { char e; };\n"
               "struct N { char c; };\n")
        got = {n: (r["align_source"]["kind"], r["align_source"].get("text"))
               for n, r in linux_json("-", stdin=src).items()}
        self.assertEqual(got, {
            "M": ("member", "d"), "A": ("attribute", "alignas(16)"),
            "P": ("packed", "__attribute__((packed))"),
            "K": ("pack", "#pragma pack(2)"), "V": ("member", "vptr"),
            "D": ("member", "base M"), "N": ("none", None)})
        self.assertIn("align 16 from alignas(16)", linux_text("--record=A", "-", stdin=src))

    def test_unaligned_members(self):
        src = ("struct __attribute__((packed)) P { char c; int i; };\n"
               "struct __attribute__((packed)) Q { int i; };\n"
               "struct S { char c; int i; };\n")
        recs = linux_json("-", stdin=src)
        self.assertTrue(named(recs["P"])["i"].get("unaligned"))
        # At offset 0, but a Q can sit at any address.
        self.assertTrue(named(recs["Q"])["i"].get("unaligned"))
        self.assertNotIn("unaligned", named(recs["S"])["i"])
        self.assertIn("int i  (unaligned)", linux_text("--record=P", "-", stdin=src))

    def test_memory_locations(self):
        src = ("struct S { unsigned a : 3, b : 5; char c; unsigned d : 2;\n"
               "  unsigned : 0; unsigned e : 1; };\n"
               "union U { unsigned x : 1; unsigned y : 2; };\n")
        recs = linux_json("-", stdin=src)
        for name, want in (("S", {"a": 1, "b": 1, "c": None, "d": 2, "e": 3}),
                           ("U", {"x": 1, "y": 2})):
            loc = {n: m.get("memory_location") for n, m in named(recs[name]).items()}
            self.assertEqual(loc, want)
        self.assertIn("unsigned int b : 5  location 1",
                      linux_text("--record=S", "-", stdin=src))

    def test_endianness(self):
        src = "struct S { unsigned a : 3; };\nstruct T { int i; };\n"
        r = lense("--json", "--target=powerpc-linux-gnu", "-", stdin=src)
        self.assertEqual(json.loads(r.stdout)["endian"], "big")
        self.assertEqual(json.loads(lense("--json", LINUX, "-", stdin=src).stdout)["endian"],
                         "little")
        text = lense("--color=false", "--target=powerpc-linux-gnu", "-", stdin=src).stdout
        headers = [l for l in text.splitlines() if l.startswith("struct")]
        self.assertTrue(headers[0].endswith(", big-endian"), headers[0])
        self.assertNotIn("endian", headers[1])  # no bitfields

    def test_non_virtual_size_in_text(self):
        src = "struct A { long a; };\nstruct B : virtual A { char b; };\n"
        self.assertIn("non-virtual size 9", linux_text("--record=B", "-", stdin=src))
        self.assertNotIn("non-virtual", linux_text("--record=A", "-", stdin=src))

    def test_traits(self):
        src = ("struct I { int a, b; };\n"
               "struct F { float f; };\n"
               "struct P { char c; int i; };\n"
               "struct C { C(const C &); int i; };\n"
               "struct V { virtual void f(); };\n")
        t = {n: tuple(r["traits"].values()) for n, r in linux_json("-", stdin=src).items()}
        self.assertEqual(t, {
            "I": (True, True, True), "F": (True, True, False),
            "P": (True, True, False), "C": (True, False, False),
            "V": (False, False, False)})
        self.assertIn("is_standard_layout yes, is_trivially_copyable yes, "
                      "has_unique_object_representations no",
                      linux_text("--record=P", "-", stdin=src))
        c = linux_json("--lang=c", "-", stdin="struct I { int a; };")
        self.assertNotIn("traits", c["I"])

    def test_cache_lines(self):
        src = "struct S { char c; char buf[100]; int tail; };\n"
        m = named(linux_json("-", stdin=src)["S"])
        self.assertTrue(m["buf"].get("crosses_cache_line"))
        self.assertNotIn("crosses_cache_line", m["tail"])
        text = linux_text("-", stdin=src)
        self.assertIn("--- cache line 1 (byte 64) ---", text)
        self.assertIn("(crosses cache line)", text)
        self.assertNotIn("cache line", linux_text("--cache-line=0", "-", stdin=src))
        wide = lense("--json", LINUX, "--cache-line=128", "-", stdin=src).stdout
        self.assertNotIn("crosses_cache_line", wide)
        self.assertEqual(json.loads(wide)["cache_line"], 128)
        self.assertEqual(lense(LINUX, "--cache-line=48", "-", stdin=src).returncode, 2)

    def test_reorder(self):
        src = ("struct S { char a; double b; char c; int d; };\n"
               "struct Tight { double b; int d; char a, c; };\n"
               "union U { char a; double b; };\n"
               "struct A { int x; };\n"
               "struct V : virtual A { char a; double b; char c; };\n"
               "struct Flex { char c; long long v; int n; char data[]; };\n"
               "struct Bits { char a; double d; unsigned x : 3, y : 3; };\n"
               "struct Base { virtual void f(); char x; };\n"
               "struct Derived : Base { double d; char e; };\n")
        recs = linux_json("-", stdin=src)
        self.assertEqual(recs["S"]["reorder"], {"size": 16, "order": [1, 0, 2, 3],
                                                "fields": ["b", "a", "c", "d"]})
        for name in ("Tight", "U", "V"):
            self.assertNotIn("reorder", recs[name], name)
        self.assertEqual(recs["Flex"]["reorder"]["fields"][-1], "data")
        # Adjacent bitfields move together.
        order = recs["Bits"]["reorder"]["order"]
        self.assertEqual(order.index(3), order.index(2) + 1)
        # Derived's own fields can go in Base's tail padding.
        self.assertEqual(recs["Derived"]["reorder"]["size"], 24)
        self.assertIn("reordering the fields saves 8 bytes (size 16): b, a, c, d",
                      linux_text("--record=S", "-", stdin=src))

    def test_compare_text(self):
        src = ("struct L { char c; long l; };\n"
               "struct I { int a, b; };\n"
               "#ifdef _WIN32\nstruct W { int w; };\n#endif\n")
        r = lense("--color=false", LINUX, "--compare=i686-linux-gnu", "-", stdin=src)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("struct I  same on both targets: size 8, align 4", r.stdout)
        row = next(l for l in r.stdout.splitlines() if l.endswith("long l"))
        self.assertTrue(row.startswith("* "), row)
        self.assertEqual(row.split()[1:7], ["8", "8", "8", "4", "4", "4"])
        self.assertIn("  x86_64-unknown-linux-gnu  size 16, align 8 from l", r.stdout)
        self.assertIn("struct W  only on x86_64-pc-windows-msvc",
                      linux_text("--compare=x86_64-pc-windows-msvc", "-", stdin=src))

    def test_compare_overrides_target_in_clang_args(self):
        r = lense("--color=false", "--compare=i686-linux-gnu", "-", "--", LINUX,
                  stdin="struct L { long l; };\n")
        self.assertIn("x86_64-unknown-linux-gnu", r.stdout)
        self.assertIn("i686-unknown-linux-gnu", r.stdout)

    def test_compare_json(self):
        src = "struct L { char c; long l; };\n"
        out = json.loads(lense("--json", LINUX, "--compare=x86_64-pc-windows-msvc", "-",
                               stdin=src).stdout)
        a, b = out["reports"]
        self.assertEqual((a["abi"], b["abi"]), ("itanium", "microsoft"))
        self.assertEqual((records(a)["L"]["size"], records(b)["L"]["size"]), (16, 8))

    # -- errors and robustness -----------------------------------------------

    def test_compile_error_keeps_valid_records(self):
        src = "struct Ok { int a; };\nstruct Bad { Missing m; int b; };\n"
        r = lense("--json", LINUX, "-", stdin=src)
        self.assertEqual(r.returncode, 1)
        self.assertIn("unknown type name 'Missing'", r.stderr)
        self.assertEqual(set(records(r.stdout)), {"Ok"})

    def test_empty_and_binary_input(self):
        self.assertEqual(lense("--json", LINUX, "-", stdin="").returncode, 0)
        r = subprocess.run([LENSE, "--json", LINUX, "-"], input=bytes(range(256)) * 4,
                           capture_output=True, timeout=120)
        self.assertIn(r.returncode, (0, 1))
        json.loads(r.stdout)

    def test_output_is_deterministic(self):
        path = self.write("t.cpp", DERIVED + "struct V : virtual Base { int v; };\n")
        runs = {lense("--json", LINUX, path).stdout for _ in range(3)}
        self.assertEqual(len(runs), 1)

    def test_text_matches_json(self):
        path = self.write("t.cpp", DERIVED)
        recs = linux_json(path)
        headers = [line for line in linux_text(path).splitlines()
                   if line.startswith("struct ")]
        self.assertEqual(len(headers), len(recs))
        for line in headers:
            name = line.split()[1]
            self.assertIn(f"size {recs[name]['size']}, align {recs[name]['align']}", line)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], "-v"])
