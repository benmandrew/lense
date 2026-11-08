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


def records(stdout: str) -> dict:
    return {r["name"]: r for r in json.loads(stdout)["records"]}


class Cli(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name: str, text: str) -> str:
        path = os.path.join(self.dir, name)
        with open(path, "w") as f:
            f.write(text)
        return path

    # -- basics -------------------------------------------------------------

    def test_help_lists_options(self):
        r = lense("--help")
        self.assertEqual(r.returncode, 0)
        for opt in ("--json", "--target", "--record", "--no-vtables",
                    "--expand-system", "--lang", "--color"):
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
        r = lense("--lang=rust", "-", stdin="")
        self.assertEqual(r.returncode, 2)

    def test_file_input(self):
        path = self.write("t.cpp", DERIVED)
        r = lense("--json", LINUX, path)
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
        path = self.write("t.c", "struct s { char c; double d; };\n")
        r = lense("--json", LINUX, path)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(records(r.stdout)["s"]["padding_bits"], 7 * 8)

    # -- options ------------------------------------------------------------

    def test_record_filter(self):
        path = self.write("t.cpp", DERIVED)
        r = lense("--json", LINUX, "--record=Derived,Other", path)
        self.assertEqual(set(records(r.stdout)), {"Derived", "Other"})
        r = lense("--json", LINUX, "--record=Nothing", path)
        self.assertEqual(r.returncode, 0)
        self.assertEqual(records(r.stdout), {})

    def test_qualified_record_name(self):
        r = lense("--json", LINUX, "--record=a::b::S", "-",
                  stdin="namespace a { namespace b { struct S { int x; }; } }")
        self.assertEqual(set(records(r.stdout)), {"a::b::S"})

    def test_no_vtables(self):
        path = self.write("t.cpp", DERIVED)
        r = lense("--json", LINUX, "--no-vtables", path)
        d = records(r.stdout)["Derived"]
        self.assertEqual(d["vtables"], [])
        self.assertNotIn("points_to", json.dumps(d))
        text = lense("--color=false", LINUX, "--no-vtables", path).stdout
        self.assertNotIn("vtable for", text)
        self.assertIn("vptr", text)

    def test_target_switches_abi(self):
        path = self.write("t.cpp", DERIVED)
        itanium = json.loads(lense("--json", LINUX, path).stdout)
        msvc = json.loads(lense("--json", MSVC, path).stdout)
        self.assertEqual(itanium["abi"], "itanium")
        self.assertEqual(itanium["target"], "x86_64-unknown-linux-gnu")
        self.assertEqual(msvc["abi"], "microsoft")
        msvc_derived = {r["name"]: r for r in msvc["records"]}["Derived"]
        self.assertEqual(len(msvc_derived["vtables"]), 2)
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

    def test_system_types_collapse_unless_expanded(self):
        src = "#include <string>\nstruct S { std::string s; bool b; };\n"
        collapsed = lense("--json", "-", stdin=src)
        self.assertEqual(collapsed.returncode, 0, collapsed.stderr)
        s = records(collapsed.stdout)["S"]["members"][0]
        self.assertEqual(s["kind"], "field")
        expanded = records(lense("--json", "--expand-system", "-", stdin=src).stdout)
        self.assertEqual(expanded["S"]["members"][0]["kind"], "record")

    def test_color(self):
        path = self.write("t.cpp", DERIVED)
        self.assertIn("\x1b[", lense("--color", LINUX, path).stdout)
        self.assertNotIn("\x1b[", lense("--color=false", LINUX, path).stdout)
        # Output to a pipe is not a terminal, so auto means no colour.
        self.assertNotIn("\x1b[", lense(LINUX, path).stdout)

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
        recs = records(lense("--json", LINUX, path).stdout)
        text = lense("--color=false", LINUX, path).stdout
        headers = [line for line in text.splitlines() if line.startswith("struct ")]
        self.assertEqual(len(headers), len(recs))
        for line in headers:
            name = line.split()[1]
            self.assertIn(f"size {recs[name]['size']}, align {recs[name]['align']}", line)

    def test_vptr_names_slot(self):
        path = self.write("t.cpp", DERIVED)
        text = lense("--color=false", LINUX, "--record=Derived", path).stdout
        self.assertIn("-> vtable for Derived[2]", text)
        self.assertIn("-> vtable for Derived[7]", text)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], "-v"])
