#!/usr/bin/env python3
"""Fuzz lense with generated programs.

Modes:
  oracle  Generate valid class hierarchies, run lense, then check its report
          against compiled code: a runtime checker on the host (one build per
          --cxx compiler), static_asserts for x86-64 Linux and Microsoft
          targets, and the structural properties in props.py. Failures are
          shrunk to a minimal program.
  mutate  Corrupt generated programs at random and check that lense never
          crashes, always prints valid JSON, and that whatever records it
          reports still satisfy the properties.

In oracle mode each worker takes --batch cases at a time and builds one
runtime checker for all of them (see oracle.batch_checker). A case that fails
in a batch is run again on its own before it is reported, so batching never
changes a result.

Every case is reproducible from its seed:
  fuzz.py --lense build/lense --cxx clang++ --seed 1234 --iterations 1
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen  # noqa: E402
import oracle  # noqa: E402
import props  # noqa: E402

CROSS = {
    "linux": "--target=x86_64-linux-gnu",
    "msvc": "--target=x86_64-pc-windows-msvc",
}


@dataclass
class Failure:
    kind: str
    detail: str
    files: Dict[str, str] = field(default_factory=dict)


@dataclass
class Outcome:
    discarded: bool = False
    failure: Optional[Failure] = None
    advisories: List[Failure] = field(default_factory=list)


@dataclass
class Prepared:
    """A case that passed every check except the runtime checker."""
    src: str
    report: dict
    files: Dict[str, str]
    out: Outcome = field(default_factory=Outcome)


class Fuzzer:
    def __init__(self, args):
        self.args = args
        self.cross = dict(CROSS) if args.static_cxx else {}

    # -- process helpers --------------------------------------------------

    def run(self, cmd: List[str], cwd: str) -> subprocess.CompletedProcess:
        try:
            return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                                  timeout=self.args.timeout)
        except subprocess.TimeoutExpired:
            return subprocess.CompletedProcess(cmd, -999, "", "timed out")

    def lense(self, path: str, cwd: str, *flags: str) -> subprocess.CompletedProcess:
        return self.run([self.args.lense, *flags, path], cwd)

    @staticmethod
    def crashed(p: subprocess.CompletedProcess) -> Optional[str]:
        if p.returncode == -999:
            return "timeout"
        if p.returncode not in (0, 1):
            return f"exit status {p.returncode}"
        return None

    def probe_cross(self) -> None:
        """Drops cross targets that the static compiler cannot handle."""
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "probe.cpp")
            with open(src, "w") as f:
                f.write("struct S { int x; };\nstatic_assert(sizeof(S) == 4, \"\");\n")
            for name, flag in list(self.cross.items()):
                p = self.run([self.args.static_cxx, "-std=c++20", "-fsyntax-only",
                              flag, src], d)
                if p.returncode != 0:
                    print(f"note: skipping static checks for {name}: "
                          f"{self.args.static_cxx} cannot compile for it",
                          file=sys.stderr)
                    del self.cross[name]

    # -- oracle mode ------------------------------------------------------

    def check_program(self, p: gen.Program, advisory: bool = True) -> Outcome:
        return self.check_programs([p], advisory)[0]

    def check_programs(self, progs: List[gen.Program],
                       advisory: bool = True) -> List[Outcome]:
        prepared = [self.prepare(p) for p in progs]
        live = [x for x in prepared if isinstance(x, Prepared)]
        compilers = [(c, False) for c in self.args.cxx]
        if advisory:
            compilers += [(c, True) for c in self.args.advisory_cxx]
        for cxx, is_advisory in compilers:
            todo = [x for x in live if x.out.failure is None]
            for x, fail in zip(todo, self.batch(cxx, todo)):
                if fail is None:
                    continue
                if is_advisory:
                    fail.kind = f"advisory:{cxx}:{fail.kind}"
                    x.out.advisories.append(fail)
                else:
                    x.out.failure = fail
        return [x.out if isinstance(x, Prepared) else x for x in prepared]

    def prepare(self, p: gen.Program):
        """Runs lense and every check that needs no new executable.

        Returns an Outcome if the case is already settled, else a Prepared.
        """
        src = gen.render(p)
        files = {"program.cpp": src}
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "program.cpp")
            with open(path, "w") as f:
                f.write(src)

            r = self.lense(path, d, "--json")
            if (why := self.crashed(r)):
                return Outcome(failure=Failure("crash", f"lense --json: {why}\n{r.stderr}", files))
            if r.returncode == 1:
                return Outcome(discarded=True)
            try:
                report = json.loads(r.stdout)
            except json.JSONDecodeError as e:
                return Outcome(failure=Failure("bad-json", str(e), files))
            files["report.json"] = r.stdout

            again = self.lense(path, d, "--json")
            if again.stdout != r.stdout:
                return Outcome(failure=Failure("nondeterministic", "two runs differ", files))
            text = self.lense(path, d, "--color=false")
            if text.returncode != 0:
                return Outcome(failure=Failure("text-mode", f"exit {text.returncode}\n{text.stderr}", files))

            errs = props.check_report(report)
            if errs:
                return Outcome(failure=Failure("property", "\n".join(errs), files))
            want = sorted(c.name for c in p.classes)
            got = sorted(rec["name"] for rec in report["records"])
            if want != got:
                return Outcome(failure=Failure("records", f"want {want}, got {got}", files))
            # Saved with failures: a standalone checker for this case alone.
            files["checker.cpp"] = oracle.runtime_checker(src, report)

            for name, flag in self.cross.items():
                rc = self.lense(path, d, "--json", flag)
                if (why := self.crashed(rc)):
                    return Outcome(failure=Failure("crash", f"lense {flag}: {why}\n{rc.stderr}", files))
                if rc.returncode == 1:
                    continue  # Valid on the host but not on this target.
                cross_report = json.loads(rc.stdout)
                files[f"report.{name}.json"] = rc.stdout
                errs = props.check_report(cross_report)
                if errs:
                    return Outcome(failure=Failure(f"property:{name}", "\n".join(errs), files))
                spath = os.path.join(d, f"static.{name}.cpp")
                static = oracle.static_checker(src, cross_report)
                files[f"static.{name}.cpp"] = static
                with open(spath, "w") as f:
                    f.write(static)
                sc = self.run([self.args.static_cxx, "-std=c++20", "-fsyntax-only",
                               "-w", flag, spath], d)
                if sc.returncode != 0:
                    return Outcome(failure=Failure(f"static:{name}", sc.stderr, files))
        return Prepared(src, report, files)

    def batch(self, cxx: str, cases: List[Prepared]) -> List[Optional[Failure]]:
        """Runtime-checks the cases with cxx, as few builds as possible."""
        results: Dict[int, Optional[Failure]] = {}
        pending = list(range(len(cases)))
        while pending:
            got = self.run_batch(cxx, [cases[i] for i in pending])
            if len(pending) > 1:
                # A broken case can corrupt its neighbours, so confirm alone.
                for k, fail in got.items():
                    if fail is not None:
                        got[k] = self.run_batch(cxx, [cases[pending[k]]])[0]
            for k, fail in got.items():
                results[pending[k]] = fail
            pending = [i for k, i in enumerate(pending) if k not in got]
        return [results[i] for i in range(len(cases))]

    def run_batch(self, cxx: str, cases: List[Prepared]) -> Dict[int, Optional[Failure]]:
        """Builds and runs one checker for the cases.

        Returns results for the cases it settled: every case if the checker
        ran to the end, else those before the crash and the crashing one.
        Always settles at least one case.
        """
        with tempfile.TemporaryDirectory() as d:
            cpath = os.path.join(d, "checker.cpp")
            with open(cpath, "w") as f:
                f.write(oracle.batch_checker([(c.src, c.report) for c in cases]))
            exe = os.path.join(d, "checker")
            c = self.run([cxx, "-std=c++20", "-O0", "-w", "-o", exe, cpath], d)
            if c.returncode != 0:
                if len(cases) == 1:
                    return {0: Failure("checker-compile", f"{cxx}\n{c.stderr}", cases[0].files)}
                return self.one_by_one(cxx, cases)
            x = self.run([exe], d)

        seen: Dict[int, List] = {}  # case -> [output lines, failures or None]
        cur = None
        for line in x.stdout.splitlines():
            if line.startswith(oracle.BEGIN):
                cur = int(line[len(oracle.BEGIN):])
                seen[cur] = [[], None]
            elif line.startswith(oracle.END):
                i, n = map(int, line[len(oracle.END):].split())
                seen[i][1] = n
                cur = None
            elif cur is not None:
                seen[cur][0].append(line)

        res: Dict[int, Optional[Failure]] = {}
        for i, (lines, n) in seen.items():
            if n == 0:
                res[i] = None
            elif n is not None:
                res[i] = Failure("runtime-mismatch", f"{cxx}: exit 1\n" +
                                 "".join(l + "\n" for l in lines), cases[i].files)
        if x.returncode != 0:
            running = [i for i, (_, n) in seen.items() if n is None]
            if running or len(cases) == 1:
                i = running[0] if running else 0
                lines = seen.get(i, [[]])[0]
                res[i] = Failure("checker-crash", f"{cxx}: exit {x.returncode}\n" +
                                 "".join(l + "\n" for l in lines) + x.stderr,
                                 cases[i].files)
        if not res:
            if len(cases) == 1:
                return {0: Failure("checker-crash", f"{cxx}: no output, exit "
                                   f"{x.returncode}\n{x.stderr}", cases[0].files)}
            return self.one_by_one(cxx, cases)
        return res

    def one_by_one(self, cxx: str, cases: List[Prepared]) -> Dict[int, Optional[Failure]]:
        return {k: self.run_batch(cxx, [c])[0] for k, c in enumerate(cases)}

    def shrink(self, p: gen.Program, still_fails: Callable[[gen.Program], bool],
               budget: int) -> gen.Program:
        progress = True
        while progress and budget > 0:
            progress = False
            for q in gen.shrink_candidates(p):
                if budget <= 0:
                    break
                if not gen.program_ok(q):
                    continue
                budget -= 1
                if still_fails(q):
                    p, progress = q, True
                    break
        return p

    def oracle_chunk(self, seeds: List[int]) -> List[Outcome]:
        progs = [gen.generate(random.Random(s), self.args.max_classes) for s in seeds]
        return [self.shrink_outcome(p, out)
                for p, out in zip(progs, self.check_programs(progs))]

    def shrink_outcome(self, p: gen.Program, out: Outcome) -> Outcome:
        if out.failure and not self.args.no_shrink:
            kind = out.failure.kind
            small = self.shrink(p, lambda q: (
                (o := self.check_program(q, advisory=False)).failure is not None
                and o.failure.kind == kind), self.args.shrink_budget)
            out.failure = self.check_program(small, advisory=False).failure or out.failure
        if out.advisories and not self.args.no_shrink and \
                self.args.advisory_shrink_budget > 0:
            shrunk = []
            for adv in out.advisories:
                kind = adv.kind
                small = self.shrink(p, lambda q: any(
                    a.kind == kind for a in self.check_program(q).advisories),
                    self.args.advisory_shrink_budget)
                res = [a for a in self.check_program(small).advisories if a.kind == kind]
                shrunk.append(res[0] if res else adv)
            out.advisories = shrunk
        return out

    # -- mutate mode ------------------------------------------------------

    TOKENS = ["{", "}", ";", ":", "::", "~", "(", ")", "[", "]", ",", "virtual",
              "struct", "union", "class", "int", "T0", "T1", "public:", "override",
              "[[no_unique_address]]", "alignas(", "template <class X>", "= 0",
              "__attribute__((packed))", "static", "operator", "...", "0", "-1",
              "decltype(", "auto", "friend", "enum", "#define", "\\"]

    def mutate(self, rng: random.Random, src: str) -> str:
        lines = src.split("\n")
        for _ in range(rng.randint(1, 4)):
            op = rng.randrange(6)
            i = rng.randrange(len(lines))
            if op == 0 and len(lines) > 1:
                del lines[i]
            elif op == 1:
                lines.insert(i, lines[rng.randrange(len(lines))])
            elif op == 2:
                j = rng.randrange(len(lines))
                lines[i], lines[j] = lines[j], lines[i]
            elif op == 3:
                words = lines[i].split(" ")
                if len(words) > 1:
                    del words[rng.randrange(len(words))]
                lines[i] = " ".join(words)
            elif op == 4:
                pos = rng.randint(0, len(lines[i]))
                lines[i] = lines[i][:pos] + " " + rng.choice(self.TOKENS) + " " + lines[i][pos:]
            else:
                lines = lines[:i + 1]
        return "\n".join(lines)

    def robust(self, src: str) -> Optional[Failure]:
        files = {"program.cpp": src}
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "program.cpp")
            with open(path, "w") as f:
                f.write(src)
            for flags in (["--json"], ["--json", CROSS["msvc"]], ["--color=false"]):
                r = self.lense(path, d, *flags)
                if (why := self.crashed(r)):
                    return Failure("crash", f"lense {' '.join(flags)}: {why}\n{r.stderr[-2000:]}", files)
                if "--json" not in flags:
                    continue
                try:
                    report = json.loads(r.stdout)
                except json.JSONDecodeError as e:
                    return Failure("bad-json", f"{flags}: {e}", files)
                errs = props.check_report(report)
                if errs:
                    files["report.json"] = r.stdout
                    return Failure("property", f"{flags}\n" + "\n".join(errs), files)
        return None

    def mutate_case(self, seed: int) -> Outcome:
        rng = random.Random(seed)
        src = self.mutate(rng, gen.render(gen.generate(rng, self.args.max_classes)))
        fail = self.robust(src)
        if fail and not self.args.no_shrink:
            # Delete lines while the same kind of failure persists.
            lines = src.split("\n")
            i = 0
            while i < len(lines):
                trial = "\n".join(lines[:i] + lines[i + 1:])
                f2 = self.robust(trial)
                if f2 and f2.kind == fail.kind:
                    lines.pop(i)
                    fail = f2
                else:
                    i += 1
        return Outcome(failure=fail)


def save(out_dir: str, mode: str, seed: int, f: Failure) -> str:
    slug = "".join(ch if ch.isalnum() else "-" for ch in f.kind.split(":")[-1])
    if f.kind.startswith("advisory:"):
        slug = "advisory-" + slug
    d = os.path.join(out_dir, f"{mode}-seed{seed}-{slug}")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    for name, content in f.files.items():
        with open(os.path.join(d, name), "w") as fh:
            fh.write(content)
    with open(os.path.join(d, "failure.txt"), "w") as fh:
        fh.write(f"{f.kind}\n{f.detail}\n")
    return d


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lense", required=True)
    ap.add_argument("--cxx", action="append", default=[],
                    help="compiler whose object code must match lense (repeatable)")
    ap.add_argument("--advisory-cxx", action="append", default=[],
                    help="compiler whose disagreements are reported, not failed")
    ap.add_argument("--static-cxx", default=None,
                    help="clang used for static checks on cross targets")
    ap.add_argument("--mode", choices=["oracle", "mutate"], default="oracle")
    ap.add_argument("--iterations", type=int, default=100)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--batch", type=int, default=16,
                    help="oracle cases per runtime checker build")
    ap.add_argument("--max-classes", type=int, default=6)
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--shrink-budget", type=int, default=300)
    ap.add_argument("--advisory-shrink-budget", type=int, default=40)
    ap.add_argument("--no-shrink", action="store_true")
    ap.add_argument("--out", default="fuzz-failures")
    args = ap.parse_args()
    # Cases run in temporary directories, so resolve relative paths now.
    def resolve(c: str) -> str:
        return os.path.abspath(c) if os.sep in c else (shutil.which(c) or c)
    args.lense = resolve(args.lense)
    args.cxx = [resolve(c) for c in args.cxx if c]
    args.advisory_cxx = [resolve(c) for c in args.advisory_cxx if c]
    if args.static_cxx:
        args.static_cxx = resolve(args.static_cxx)
    if args.mode == "oracle" and not args.cxx:
        ap.error("oracle mode needs at least one --cxx")
    if args.static_cxx is None and args.cxx:
        args.static_cxx = args.cxx[0]
    seed0 = args.seed if args.seed is not None else int(time.time())

    fz = Fuzzer(args)
    if args.mode == "oracle":
        fz.probe_cross()
    seeds = [seed0 + i for i in range(args.iterations)]
    if args.mode == "oracle":
        run_chunk = fz.oracle_chunk
        # Smaller batches when there are too few cases to keep every job busy.
        size = max(1, min(args.batch, -(-len(seeds) // args.jobs)))
    else:
        run_chunk = lambda chunk: [fz.mutate_case(s) for s in chunk]  # noqa: E731
        size = 1
    chunks = [seeds[i:i + size] for i in range(0, len(seeds), size)]

    failures = discarded = 0
    advisories: Dict[str, int] = {}
    start = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = ((seed, out) for chunk, outs in zip(chunks, pool.map(run_chunk, chunks))
                   for seed, out in zip(chunk, outs))
        for seed, out in results:
            discarded += out.discarded
            if out.failure:
                failures += 1
                d = save(args.out, args.mode, seed, out.failure)
                first = out.failure.detail.strip().splitlines()[:6]
                print(f"FAIL seed {seed}: {out.failure.kind} (saved to {d})")
                for line in first:
                    print(f"    {line}")
            for adv in out.advisories:
                advisories[adv.kind] = advisories.get(adv.kind, 0) + 1
                d = save(args.out, args.mode, seed, adv)
                print(f"note seed {seed}: {adv.kind} (saved to {d})")

    took = time.time() - start
    summary = (f"{args.mode}: {len(seeds)} cases from seed {seed0} in {took:.1f}s, "
               f"{discarded} discarded, {failures} failed")
    if advisories:
        summary += ", advisory: " + ", ".join(f"{k} x{v}" for k, v in sorted(advisories.items()))
    print(summary)
    if args.mode == "oracle" and discarded > len(seeds) // 2:
        print("FAIL more than half the generated programs were rejected; "
              "the generator is broken")
        return 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
