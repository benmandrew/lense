"""Random C++ programs for property-based testing of lense.

A Program is a list of classes T0..Tn, each able to use earlier ones as bases
or member types. The generator only builds programs that compile and that the
oracle can check:

- every class is default-constructible (no pure virtuals, no references);
- no base cast is ambiguous: a class never has a direct base that is also an
  indirect base, and no type is both a virtual and a non-virtual base;
- a class with two or more bases overrides every virtual function it inherits,
  so each function has a unique final overrider;
- union members are trivial.

Each virtual function Tc::mk returns 1000 * c + k and records the address of
its complete object, so the oracle can call vtable slots and check both.
"""

from __future__ import annotations

import copy
import random
from dataclasses import dataclass, field
from typing import List, Optional, Set, Tuple

SCALARS = [
    "char", "signed char", "unsigned char", "short", "int", "unsigned",
    "long", "long long", "float", "double", "long double", "bool",
    "void *", "int *",
]
# (type, maximum width)
BITFIELD_TYPES = [
    ("unsigned", 32), ("int", 32), ("unsigned char", 8), ("short", 16),
    ("unsigned short", 16), ("unsigned long long", 64), ("long long", 64),
]
N_METHODS = 6


@dataclass
class Field:
    kind: str  # scalar | array | bitfield | record | record_array
    type: str
    name: Optional[str]  # None for an unnamed bitfield
    width: int = 0
    count: int = 0
    nua: bool = False  # [[no_unique_address]]
    align: int = 0  # alignas on the member


@dataclass
class Class:
    name: str
    union: bool = False
    bases: List[Tuple[str, bool]] = field(default_factory=list)  # (name, virtual)
    fields: List[Field] = field(default_factory=list)
    methods: List[int] = field(default_factory=list)
    vdtor: bool = False
    align: int = 0
    packed: bool = False


@dataclass
class Program:
    classes: List[Class]

    def get(self, name: str) -> Class:
        for c in self.classes:
            if c.name == name:
                return c
        raise KeyError(name)


def class_index(name: str) -> int:
    return int(name[1:])


# ---- Derived properties -----------------------------------------------------

def edges(p: Program, name: str) -> List[Tuple[str, bool]]:
    """Every (base, virtual) edge in the hierarchy below `name`."""
    out = []
    for b, v in p.get(name).bases:
        out.append((b, v))
        out.extend(edges(p, b))
    return out


def reach(p: Program, name: str) -> Set[str]:
    return {b for b, _ in edges(p, name)}


def methods_available(p: Program, name: str) -> Set[int]:
    out = set(p.get(name).methods)
    for b in reach(p, name):
        out |= set(p.get(b).methods)
    return out


def is_dynamic(p: Program, name: str) -> bool:
    c = p.get(name)
    if c.methods or c.vdtor or any(v for _, v in c.bases):
        return True
    return any(is_dynamic(p, b) for b, _ in c.bases)


def is_trivial(p: Program, name: str) -> bool:
    c = p.get(name)
    if is_dynamic(p, name):
        return False
    if any(not is_trivial(p, b) for b, _ in c.bases):
        return False
    return all(f.kind not in ("record", "record_array") or is_trivial(p, f.type)
               for f in c.fields)


def bases_ok(p: Program, bases: List[Tuple[str, bool]]) -> bool:
    names = [b for b, _ in bases]
    if len(set(names)) != len(names):
        return False
    for i, (b, v) in enumerate(bases):
        others = [o for j, (o, _) in enumerate(bases) if j != i]
        if not v and any(b in reach(p, o) for o in others):
            return False
    all_edges = list(bases)
    for b, _ in bases:
        all_edges.extend(edges(p, b))
    virt = {b for b, v in all_edges if v}
    nonvirt = {b for b, v in all_edges if not v}
    return not (virt & nonvirt)


def program_ok(p: Program) -> bool:
    """Re-checks the generator's rules; shrinking can break them."""
    names = {c.name for c in p.classes}
    for i, c in enumerate(p.classes):
        earlier = {d.name for d in p.classes[:i]}
        if any(b not in earlier for b, _ in c.bases):
            return False
        for f in c.fields:
            if f.kind in ("record", "record_array") and f.type not in earlier:
                return False
        if c.union and (c.bases or c.methods or c.vdtor):
            return False
        if c.union and any(f.kind in ("record", "record_array") and
                           not is_trivial(p, f.type) for f in c.fields):
            return False
        if any(p.get(b).union for b, _ in c.bases):
            return False
        if not bases_ok(p, c.bases):
            return False
    return bool(names)


# ---- Generation -------------------------------------------------------------

def gen_field(rng: random.Random, p: Program, cls: Class, idx: int) -> Field:
    name = f"f{idx}"
    candidates = [c.name for c in p.classes if not c.union or rng.random() < 0.5]
    if cls.union:
        candidates = [n for n in candidates if is_trivial(p, n)]
    roll = rng.random()
    if not candidates:
        roll *= 0.7  # Only non-record kinds.
    if roll < 0.35:
        f = Field("scalar", rng.choice(SCALARS), name)
    elif roll < 0.5:
        f = Field("array", rng.choice(SCALARS), name, count=rng.randint(1, 5))
    elif roll < 0.7:
        t, maxw = rng.choice(BITFIELD_TYPES)
        if rng.random() < 0.15:
            return Field("bitfield", t, None, width=rng.randint(0, maxw))
        f = Field("bitfield", t, name, width=rng.randint(1, maxw))
        return f
    elif roll < 0.9:
        f = Field("record", rng.choice(candidates), name)
        f.nua = not cls.union and rng.random() < 0.25
    else:
        f = Field("record_array", rng.choice(candidates), name,
                  count=rng.randint(1, 3))
    if f.kind != "bitfield" and not cls.union and rng.random() < 0.08:
        f.align = rng.choice([16, 32])
    return f


def generate(rng: random.Random, max_classes: int = 6) -> Program:
    p = Program([])
    for i in range(rng.randint(1, max_classes)):
        c = Class(f"T{i}")
        bases_pool = [d.name for d in p.classes if not d.union]
        c.union = i > 0 and rng.random() < 0.1
        p.classes.append(c)  # visible to the helpers below; never to itself
        if not c.union:
            for b in rng.sample(bases_pool, k=min(len(bases_pool),
                                                 rng.choice([0, 0, 1, 1, 2, 3]))):
                trial = c.bases + [(b, rng.random() < 0.3)]
                if bases_ok(p, trial):
                    c.bases = trial
            if rng.random() < 0.6:
                c.methods = sorted(rng.sample(range(N_METHODS),
                                              k=rng.randint(1, 3)))
            c.vdtor = rng.random() < 0.3
            if rng.random() < 0.1:
                c.align = rng.choice([16, 32, 64])
            elif rng.random() < 0.08:
                c.packed = True
        # Fields may not use the class itself.
        p.classes.pop()
        c.fields = [gen_field(rng, p, c, k) for k in range(rng.randint(0, 5))]
        p.classes.append(c)
        if len(c.bases) >= 2:
            inherited = set()
            for b, _ in c.bases:
                inherited |= methods_available(p, b)
            c.methods = sorted(set(c.methods) | inherited)
    return p


# ---- Rendering --------------------------------------------------------------

def render_field(f: Field) -> str:
    pre = ""
    if f.nua:
        pre += "[[no_unique_address]] "
    if f.align:
        pre += f"alignas({f.align}) "
    if f.kind == "bitfield":
        return f"  {f.type} {f.name or ''} : {f.width};"
    suffix = f"[{f.count}]" if f.kind in ("array", "record_array") else ""
    t = f.type
    if t.endswith("*"):
        return f"  {pre}{t}{f.name}{suffix};"
    return f"  {pre}{t} {f.name}{suffix};"


def render(p: Program) -> str:
    out = ["// Generated by tests/fuzz/gen.py.", "void *lense_top;", ""]
    for c in p.classes:
        head = "union" if c.union else "struct"
        if c.align:
            head += f" alignas({c.align})"
        if c.packed:
            head += " __attribute__((packed))"
        head += f" {c.name}"
        if c.bases:
            head += " : " + ", ".join(
                ("virtual " if v else "") + b for b, v in c.bases)
        out.append(head + " {")
        if c.vdtor:
            out.append(f"  virtual ~{c.name}() {{}}")
        for k in c.methods:
            ident = 1000 * class_index(c.name) + k
            out.append(f"  virtual int m{k}() {{ lense_top = "
                       f"dynamic_cast<void *>(this); return {ident}; }}")
        out.extend(render_field(f) for f in c.fields)
        out.append("};")
        out.append("")
    return "\n".join(out)


# ---- Shrinking --------------------------------------------------------------

def shrink_candidates(p: Program):
    """Smaller variants of p, roughly largest reduction first."""
    for i in reversed(range(len(p.classes))):
        q = copy.deepcopy(p)
        gone = q.classes.pop(i).name
        for c in q.classes:
            c.bases = [(b, v) for b, v in c.bases if b != gone]
            c.fields = [f for f in c.fields
                        if f.kind not in ("record", "record_array") or f.type != gone]
        yield q
    for i, c in enumerate(p.classes):
        for j in range(len(c.bases)):
            q = copy.deepcopy(p)
            del q.classes[i].bases[j]
            yield q
        for j in range(len(c.fields)):
            q = copy.deepcopy(p)
            del q.classes[i].fields[j]
            yield q
        for j in range(len(c.methods)):
            q = copy.deepcopy(p)
            del q.classes[i].methods[j]
            yield q
        for attr, empty in (("vdtor", False), ("align", 0), ("packed", False)):
            if getattr(c, attr) != empty:
                q = copy.deepcopy(p)
                setattr(q.classes[i], attr, empty)
                yield q
        for j, f in enumerate(c.fields):
            for attr, empty in (("nua", False), ("align", 0)):
                if getattr(f, attr) != empty:
                    q = copy.deepcopy(p)
                    setattr(q.classes[i].fields[j], attr, empty)
                    yield q
            if f.kind in ("array", "record_array") and f.count > 1:
                q = copy.deepcopy(p)
                q.classes[i].fields[j].count = 1
                yield q
            if f.kind == "bitfield" and f.width > 1:
                q = copy.deepcopy(p)
                q.classes[i].fields[j].width = f.width // 2 or 1
                yield q
