"""Properties every lense JSON report must satisfy, whatever the input.

These need no compiler, so they also run on targets that cannot execute here
and on the mutated (often invalid) programs used for robustness fuzzing.
"""

from __future__ import annotations

import math
from typing import Iterator, List, Tuple

LEAVES = {"field", "bitfield", "vptr", "vbptr", "vtordisp"}
DYNAMIC = {"vptr", "vbptr", "vtordisp"}
ALIGN_SOURCES = {"none", "member", "attribute", "packed", "pack"}
TRAITS = ("standard_layout", "trivially_copyable", "unique_object_representations")


def bits(n: dict) -> Tuple[int, int]:
    """(start, end) of a node or hole, in bits."""
    if "bit_offset" in n:
        return n["bit_offset"], n["bit_offset"] + n["bit_size"]
    return n["offset"] * 8, (n["offset"] + n["size"]) * 8


def walk(members: List[dict]) -> Iterator[dict]:
    for n in members:
        yield n
        if n["kind"] == "record":
            yield from walk(n["members"])


def pow2(n) -> bool:
    return isinstance(n, int) and n > 0 and not n & (n - 1)


def direct_fields(r: dict) -> List[dict]:
    """The record's non-static data members in declaration order, as
    clang's RecordDecl::fields() gives them (unnamed bitfields included)."""
    return [n for n in r["members"]
            if n["kind"] in ("field", "bitfield") or
            (n["kind"] == "record" and n["role"] == "field")]


def check_annotations(r: dict, cache_line: int, err) -> None:
    """align, unaligned, crosses_cache_line and memory_location on nodes."""
    ralign = r["align"]
    line = cache_line * 8
    for n in walk(r["members"]):
        kind = n["kind"]
        s, e = bits(n)
        what = f"{kind} {n.get('name', n.get('type', ''))} at bit {s}"
        if kind in ("bitfield", "padding"):
            if "align" in n:
                err(f"{what} has an align")
        elif "align" not in n:
            err(f"{what} has no align")
        elif not pow2(n["align"]):
            err(f"{what}: align {n['align']} is not a power of two")
        # unaligned: the record's alignment and the offset do not guarantee
        # what the member asks for. gcd(0, a) is a.
        a = n.get("align", 0)
        want = a > 1 and e > s and math.gcd(n["offset"], ralign) < a
        if n.get("unaligned", False) != want:
            err(f"{what}: unaligned is {n.get('unaligned', False)}, but "
                f"gcd({n['offset']}, {ralign}) vs align {a} says {want}")
        # crosses_cache_line: a leaf whose first and last bit lie in
        # different lines, counted from the start of the record.
        want = kind in LEAVES and line > 0 and e > s and s // line != (e - 1) // line
        if n.get("crosses_cache_line", False) != want:
            err(f"{what}: crosses_cache_line is "
                f"{n.get('crosses_cache_line', False)}, want {want}")
        if "memory_location" in n and (kind != "bitfield" or e == s):
            err(f"{what} has a memory location")
        if kind == "bitfield" and e > s and "memory_location" not in n:
            err(f"{what} has no memory location")

    # Memory locations: a maximal run of adjacent nonzero-width bitfields in
    # a struct or class shares one number; a union bitfield gets its own.
    # Padding does not split a run (it is not a member); numbers go 1, 2, ...
    # in pre-order.
    want = {}
    count = [0]

    def number(members: List[dict], union: bool) -> None:
        run = False
        for n in members:
            if n["kind"] == "padding":
                continue
            if n["kind"] == "bitfield" and n["bit_size"]:
                if union or not run:
                    count[0] += 1
                want[id(n)] = count[0]
                run = not union
            else:
                run = False
            if n["kind"] == "record":
                number(n["members"], n.get("tag") == "union")
    number(r["members"], r.get("tag") == "union")
    got = [n["memory_location"] for n in walk(r["members"]) if "memory_location" in n]
    if got and (got[0] != 1 or any(b - a not in (0, 1) for a, b in zip(got, got[1:]))):
        err(f"memory locations {got} do not count up from 1 in order")
    for n in walk(r["members"]):
        if id(n) in want and n.get("memory_location") != want[id(n)]:
            err(f"bitfield {n.get('name', '')} at bit {n['bit_offset']}: memory "
                f"location {n.get('memory_location')}, want {want[id(n)]}")


def check_extras(r: dict, err) -> None:
    """align_source, traits and reorder."""
    src = r.get("align_source")
    if src is not None:
        if src.get("kind") not in ALIGN_SOURCES:
            err(f"unknown align_source kind {src.get('kind')}")
        elif src["kind"] == "none" and "text" in src:
            err("align_source none has a text")
        elif src["kind"] != "none" and not src.get("text"):
            err(f"align_source {src['kind']} has no text")

    tr = r.get("traits")
    dynamic = any(n["kind"] in DYNAMIC for n in walk(r["members"]))
    if tr is not None:
        if sorted(tr) != sorted(TRAITS) or not all(isinstance(tr[k], bool) for k in TRAITS):
            err(f"bad traits {tr}")
        else:
            # has_unique_object_representations needs a trivially copyable
            # type with no padding; a vptr makes a class neither trivially
            # copyable nor standard-layout. Clang sizes a union's bitfield
            # by its type, so `union { unsigned x : 12; }` counts as having
            # unique representations despite its padding.
            union_bits = r.get("tag") == "union" and any(
                n["kind"] == "bitfield" for n in r["members"])
            if tr["unique_object_representations"] and (
                    not tr["trivially_copyable"] or
                    (r["padding_bits"] and not union_bits)):
                err(f"unique object representations with {r['padding_bits']} "
                    f"padding bits, trivially copyable {tr['trivially_copyable']}")
            if dynamic and (tr["standard_layout"] or tr["trivially_copyable"]):
                err(f"dynamic record has traits {tr}")

    ro = r.get("reorder")
    if ro is None:
        return
    if r.get("tag") == "union":
        err("reorder offered for a union")
    if any(n["kind"] == "record" and n["role"] in ("virtual_base", "primary_virtual_base")
           for n in r["members"]):
        err("reorder offered for a record with virtual bases")
    if not 0 < ro["size"] < r["size"]:
        err(f"reorder size {ro['size']} is not in 1..{r['size'] - 1}")
    if ro["size"] % r["align"]:
        err(f"reorder size {ro['size']} is not a multiple of {r['align']}")
    # Reordering only moves padding about, so cannot save more than there is.
    if r["size"] - ro["size"] > r["padding_bits"] // 8:
        err(f"reorder saves {r['size'] - ro['size']} bytes, more than the "
            f"{r['padding_bits'] // 8} bytes of padding")
    fields = direct_fields(r)
    order = ro["order"]
    if sorted(order) != list(range(len(fields))):
        err(f"reorder order {order} is not a permutation of {len(fields)} fields")
        return
    if order == sorted(order):
        err("reorder order is the declaration order")
    if len(ro["fields"]) != len(order):
        err(f"reorder has {len(order)} indices but fields {ro['fields']}")
        return
    for k, i in enumerate(order):
        if "name" in fields[i] and ro["fields"][k] != fields[i]["name"]:
            err(f"reorder.fields[{k}] is {ro['fields'][k]}, field {i} is "
                f"{fields[i]['name']}")
    if fields and fields[-1].get("type", "").endswith("[]") and \
            order[-1] != len(fields) - 1:
        err("reorder moves the flexible array member")


def check_record(r: dict, cache_line: int = 0) -> List[str]:
    errs: List[str] = []
    name = r["name"]
    size_bits = r["size"] * 8

    def err(msg: str) -> None:
        errs.append(f"{name}: {msg}")

    if r["align"] <= 0 or r["align"] & (r["align"] - 1):
        err(f"alignment {r['align']} is not a power of two")
        return errs
    if r["size"] % r["align"]:
        err(f"size {r['size']} is not a multiple of alignment {r['align']}")
    if r["data_size"] > r["size"] or r["nv_size"] > r["size"]:
        err("data size or non-virtual size exceeds size")

    # Holes are sorted, disjoint, inside the record, and add up.
    holes = [bits(h) for h in r["holes"]]
    for (s, e) in holes:
        if not (0 <= s < e <= size_bits):
            err(f"hole {s}..{e} outside 0..{size_bits}")
    for (a, b) in zip(holes, holes[1:]):
        if a[1] > b[0]:
            err(f"holes {a} and {b} overlap or are unsorted")
    if sum(e - s for s, e in holes) != r["padding_bits"]:
        err("padding_bits does not match the holes")

    # Padding nodes in the tree are exactly the holes.
    tree_pad = sorted(bits(n) for n in walk(r["members"]) if n["kind"] == "padding")
    if tree_pad != holes:
        err(f"padding nodes {tree_pad} differ from holes {holes}")

    # Every bit is a hole or belongs to a leaf or an empty member; no hole
    # touches a leaf.
    used = [bits(n) for n in walk(r["members"])
            if n["kind"] in LEAVES or (n["kind"] == "record" and n.get("empty"))]
    for (s, e) in holes:
        for (a, b) in used:
            if a < e and s < b and b > a:
                err(f"hole {s}..{e} overlaps member {a}..{b}")
    covered = sorted(used + holes)
    pos = 0
    for (s, e) in covered:
        if s > pos:
            err(f"bits {pos}..{s} are neither padding nor a member")
        pos = max(pos, e)
    if pos < size_bits:
        err(f"bits {pos}..{size_bits} are neither padding nor a member")

    # Children sit inside their parent (empty records may sit anywhere).
    def nest(parent: dict) -> None:
        ps, pe = bits(parent)
        for c in parent["members"]:
            cs, ce = bits(c)
            if not parent.get("empty") and (cs < ps or ce > pe):
                err(f"{c['kind']} {c.get('name', c.get('type', ''))} at "
                    f"{cs}..{ce} escapes {parent.get('type')} at {ps}..{pe}")
            if c["kind"] == "record":
                nest(c)
    nest({"kind": "record", "type": name, "offset": 0, "size": r["size"],
          "members": r["members"]})

    check_annotations(r, cache_line, err)
    check_extras(r, err)
    return errs


def check_report(report: dict) -> List[str]:
    errs: List[str] = []
    if report.get("abi") not in ("itanium", "microsoft"):
        errs.append(f"unknown abi {report.get('abi')}")
    if report.get("endian") not in ("little", "big"):
        errs.append(f"unknown endian {report.get('endian')}")
    cache_line = report.get("cache_line", 0)
    if "cache_line" in report and not pow2(cache_line):
        errs.append(f"cache_line {cache_line} is not a power of two")
    names = [r["name"] for r in report["records"]]
    if len(set(names)) != len(names):
        errs.append(f"duplicate records in {names}")
    for r in report["records"]:
        errs.extend(check_record(r, cache_line))
    return errs
