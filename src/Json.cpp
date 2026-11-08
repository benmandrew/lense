#include "Json.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

namespace lense {
namespace {

const char *kindName(NodeKind k) {
  switch (k) {
  case NodeKind::Record: return "record";
  case NodeKind::Field: return "field";
  case NodeKind::BitField: return "bitfield";
  case NodeKind::VPtr: return "vptr";
  case NodeKind::VBPtr: return "vbptr";
  case NodeKind::VtorDisp: return "vtordisp";
  case NodeKind::Padding: return "padding";
  }
  return "";
}

const char *roleName(Role r) {
  switch (r) {
  case Role::Root: return "root";
  case Role::Field: return "field";
  case Role::PrimaryBase: return "primary_base";
  case Role::Base: return "base";
  case Role::PrimaryVirtualBase: return "primary_virtual_base";
  case Role::VirtualBase: return "virtual_base";
  }
  return "";
}

const char *entryName(EntryKind k) {
  switch (k) {
  case EntryKind::VCallOffset: return "vcall_offset";
  case EntryKind::VBaseOffset: return "vbase_offset";
  case EntryKind::OffsetToTop: return "offset_to_top";
  case EntryKind::RTTI: return "rtti";
  case EntryKind::Function: return "function";
  case EntryKind::CompleteDtor: return "complete_dtor";
  case EntryKind::DeletingDtor: return "deleting_dtor";
  case EntryKind::UnusedFunction: return "unused_function";
  }
  return "";
}

// json::OStream checks every string for valid UTF-8 and escapes it a byte
// at a time. Names and types are almost always printable ASCII without
// quotes or backslashes, which need neither, so write those directly. The
// output is the same either way.
void attr(llvm::json::OStream &J, llvm::StringRef key, llvm::StringRef s) {
  bool plain = llvm::all_of(s, [](unsigned char c) {
    return c >= 0x20 && c < 0x80 && c != '"' && c != '\\';
  });
  if (!plain) {
    J.attribute(key, s);
    return;
  }
  J.attributeBegin(key);
  J.rawValue([&](llvm::raw_ostream &os) { os << '"' << s << '"'; });
  J.attributeEnd();
}

// Offsets and sizes are written in bytes. Anything that does not fall on a
// byte boundary also gets its exact position in bits.
void position(llvm::json::OStream &J, uint64_t offsetBits, uint64_t sizeBits,
              bool forceBits) {
  J.attribute("offset", offsetBits / 8);
  if (forceBits || offsetBits % 8 || sizeBits % 8) {
    J.attribute("bit_offset", offsetBits);
    J.attribute("bit_size", sizeBits);
  } else {
    J.attribute("size", sizeBits / 8);
  }
}

void node(llvm::json::OStream &J, const Node &n) {
  J.object([&] {
    attr(J, "kind", kindName(n.kind));
    if (n.kind == NodeKind::Record)
      attr(J, "role", roleName(n.role));
    position(J, n.offsetBits, n.sizeBits, n.kind == NodeKind::BitField);
    if (!n.tag.empty())
      attr(J, "tag", n.tag);
    if (!n.type.empty())
      attr(J, "type", n.type);
    if (!n.name.empty())
      attr(J, "name", n.name);
    if (n.empty)
      J.attribute("empty", true);
    if (n.vtable)
      J.attributeObject("points_to", [&] {
        attr(J, "table", n.vtable->table);
        J.attribute("slot", n.vtable->slot);
      });
    if (n.kind == NodeKind::Record)
      J.attributeArray("members", [&] {
        for (const Node &c : n.children)
          node(J, c);
      });
  });
}

void vtable(llvm::json::OStream &J, const VTable &vt) {
  J.object([&] {
    attr(J, "name", vt.name);
    if (vt.vfptrOffset)
      J.attribute("vfptr_offset", *vt.vfptrOffset);
    if (!vt.tableStarts.empty())
      J.attributeArray("table_starts", [&] {
        for (unsigned s : vt.tableStarts)
          J.value(s);
      });
    J.attributeArray("address_points", [&] {
      for (const AddressPoint &ap : vt.addressPoints)
        J.object([&] {
          J.attribute("index", ap.index);
          J.attributeArray("subobjects", [&] {
            for (const Subobject &s : ap.subobjects)
              J.object([&] {
                attr(J, "type", s.type);
                J.attribute("offset", s.offset);
              });
          });
        });
    });
    J.attributeArray("entries", [&] {
      for (size_t i = 0; i < vt.entries.size(); ++i) {
        const VTableEntry &e = vt.entries[i];
        J.object([&] {
          J.attribute("index", i);
          attr(J, "kind", entryName(e.kind));
          switch (e.kind) {
          case EntryKind::VCallOffset:
          case EntryKind::VBaseOffset:
          case EntryKind::OffsetToTop:
            J.attribute("value", e.value);
            break;
          case EntryKind::RTTI:
            attr(J, "type", e.text);
            break;
          default:
            attr(J, "function", e.text);
            if (e.pure)
              J.attribute("pure", true);
          }
          if (e.thunk)
            J.attributeObject("thunk", [&] {
              const Thunk &t = *e.thunk;
              J.attribute("this_adjustment", t.thisAdjust);
              if (t.vcallOffsetAt)
                J.attribute("vcall_offset_offset", t.vcallOffsetAt);
              if (t.virtualThis)
                J.attribute("virtual_this_adjustment", true);
              if (t.returnAdjust)
                J.attribute("return_adjustment", t.returnAdjust);
              if (t.vbaseOffsetAt)
                J.attribute("vbase_offset_offset", t.vbaseOffsetAt);
              if (t.virtualReturn)
                J.attribute("virtual_return_adjustment", true);
            });
        });
      }
    });
  });
}

} // namespace

void printJson(llvm::raw_ostream &os, const Report &report) {
  llvm::json::OStream J(os, 2);
  J.object([&] {
    attr(J, "target", report.target);
    attr(J, "abi", report.abi);
    J.attribute("pointer_size", report.pointerBits / 8);
    J.attributeArray("records", [&] {
      for (const Record &r : report.records)
        J.object([&] {
          attr(J, "name", r.name);
          attr(J, "tag", r.tag);
          J.attributeObject("location", [&] {
            J.attribute("line", r.line);
            J.attribute("column", r.column);
          });
          J.attribute("size", r.size);
          J.attribute("align", r.align);
          J.attribute("data_size", r.dataSize);
          J.attribute("nv_size", r.nvSize);
          J.attribute("nv_align", r.nvAlign);
          J.attribute("padding_bits", r.paddingBits);
          if (!r.source.empty())
            J.attributeObject("source", [&] {
              J.attribute("line", r.sourceLine);
              attr(J, "text", r.source);
            });
          J.attributeArray("holes", [&] {
            for (const Hole &h : r.holes)
              J.object([&] { position(J, h.offsetBits, h.sizeBits, false); });
          });
          J.attributeArray("members", [&] {
            for (const Node &c : r.layout.children)
              node(J, c);
          });
          J.attributeArray("vtables", [&] {
            for (const VTable &vt : r.vtables)
              vtable(J, vt);
          });
        });
    });
  });
  os << '\n';
}

} // namespace lense
