"""Properties every lense JSON report must satisfy, whatever the input.

These need no compiler, so they also run on targets that cannot execute here
and on the mutated (often invalid) programs used for robustness fuzzing.
"""

from __future__ import annotations

from typing import Iterator, List, Tuple

LEAVES = {"field", "bitfield", "vptr", "vbptr", "vtordisp"}


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


def check_record(r: dict, abi: str) -> List[str]:
    errs: List[str] = []
    name = r["name"]
    size_bits = r["size"] * 8

    def err(msg: str) -> None:
        errs.append(f"{name}: {msg}")

    if r["align"] <= 0 or r["align"] & (r["align"] - 1):
        err(f"alignment {r['align']} is not a power of two")
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

    # Vtables are well formed.
    tables = {t["name"]: t for t in r["vtables"]}
    for t in r["vtables"]:
        entries = t["entries"]
        if [e["index"] for e in entries] != list(range(len(entries))):
            err(f"{t['name']}: entry indices are not 0..n-1")
        starts = t.get("table_starts", [])
        if starts and (starts[0] != 0 or starts != sorted(set(starts))):
            err(f"{t['name']}: bad table starts {starts}")
        for ap in t["address_points"]:
            i = ap["index"]
            if not 0 <= i <= len(entries):
                err(f"{t['name']}: address point {i} out of range")
                continue
            if abi == "itanium":
                # [offset_to_top, RTTI] sit just before every address point.
                if i < 2 or entries[i - 1]["kind"] != "rtti" or \
                        entries[i - 2]["kind"] != "offset_to_top":
                    err(f"{t['name']}: no offset_to_top/RTTI before {i}")
                    continue
                for sub in ap["subobjects"]:
                    if entries[i - 2]["value"] != -sub["offset"]:
                        err(f"{t['name']}: offset_to_top at {i - 2} is "
                            f"{entries[i - 2]['value']}, subobject {sub['type']} "
                            f"is at {sub['offset']}")
        if abi == "itanium":
            # An address point belongs to the table holding the RTTI entry
            # just before it; it may equal the next table's start.
            for k in range(len(starts)):
                end = starts[k + 1] if k + 1 < len(starts) else len(entries)
                n = sum(1 for ap in t["address_points"]
                        if starts[k] <= ap["index"] - 1 < end)
                if n != 1:
                    err(f"{t['name']}: table at {starts[k]} has {n} address points")

    # Vptrs of the record itself point at one of its address points, keyed by
    # the subobject that owns the vptr.
    def vptrs(members: List[dict], in_field: bool) -> Iterator[Tuple[dict, bool]]:
        for n in members:
            if n["kind"] == "vptr":
                yield n, in_field
            elif n["kind"] == "record":
                yield from vptrs(n["members"], in_field or n["role"] == "field")
    for v, in_field in vptrs(r["members"], False):
        pt = v.get("points_to")
        if in_field or pt is None:
            continue
        t = tables.get(pt["table"])
        if t is None:
            err(f"vptr at {v['offset']} points at unknown table {pt['table']}")
            continue
        ap = [a for a in t["address_points"] if a["index"] == pt["slot"]]
        if not ap:
            err(f"vptr at {v['offset']} points at slot {pt['slot']}, "
                f"which is not an address point")
        elif abi == "itanium" and not any(
                s["type"] == v["type"] and s["offset"] == v["offset"]
                for s in ap[0]["subobjects"]):
            err(f"vptr of {v['type']} at {v['offset']} is not a subobject of "
                f"its address point")
        elif abi == "microsoft" and t.get("vfptr_offset") != v["offset"]:
            err(f"vfptr at {v['offset']} points at a table for offset "
                f"{t.get('vfptr_offset')}")
    return errs


def check_report(report: dict) -> List[str]:
    errs: List[str] = []
    if report.get("abi") not in ("itanium", "microsoft"):
        errs.append(f"unknown abi {report.get('abi')}")
    names = [r["name"] for r in report["records"]]
    if len(set(names)) != len(names):
        errs.append(f"duplicate records in {names}")
    for r in report["records"]:
        errs.extend(check_record(r, report["abi"]))
    return errs
