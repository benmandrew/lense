#include "Text.h"

#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <string>

namespace lense {
namespace {

using Color = llvm::raw_ostream::Colors;

std::string fmtOffset(uint64_t bits) {
  std::string s = std::to_string(bits / 8);
  if (bits % 8)
    s += ":" + std::to_string(bits % 8);
  return s;
}

std::string fmtSize(uint64_t bits, bool inBits) {
  if (!inBits && bits % 8 == 0)
    return std::to_string(bits / 8);
  return std::to_string(bits) + "b";
}

std::string fmtAmount(uint64_t bits) {
  if (bits % 8 == 0)
    return std::to_string(bits / 8) + (bits == 8 ? " byte" : " bytes");
  return std::to_string(bits) + " bits";
}

struct Line {
  std::string offset;
  std::string size;
  unsigned depth;
  const Node *node;
};

void collect(const Node &n, unsigned depth, std::vector<Line> &out) {
  bool inBits = n.kind == NodeKind::BitField ||
                (n.kind == NodeKind::Padding &&
                 (n.offsetBits % 8 || n.sizeBits % 8));
  out.push_back({fmtOffset(n.offsetBits), fmtSize(n.sizeBits, inBits), depth,
                 &n});
  if (n.kind == NodeKind::Record)
    for (const Node &c : n.children)
      collect(c, depth + 1, out);
}

class Printer {
public:
  Printer(llvm::raw_ostream &os, bool ms) : os(os), ms(ms) {}

  void record(const Record &r) {
    bold();
    os << r.tag << ' ' << r.name;
    reset();
    dim();
    os << "  size " << r.size << ", align " << r.align;
    if (r.dataSize != r.size)
      os << ", data size " << r.dataSize;
    if (r.paddingBits)
      os << ", padding " << fmtAmount(r.paddingBits);
    reset();
    os << '\n';

    std::vector<Line> lines;
    for (const Node &c : r.layout.children)
      collect(c, 0, lines);
    size_t offW = 6, sizeW = 4;
    for (const Line &l : lines) {
      offW = std::max(offW, l.offset.size());
      sizeW = std::max(sizeW, l.size.size());
    }
    dim();
    os << "  " << std::string(offW - 6, ' ') << "offset  "
       << std::string(sizeW - 4, ' ') << "size\n";
    reset();
    for (const Line &l : lines) {
      os << "  " << std::string(offW - l.offset.size(), ' ') << l.offset
         << "  " << std::string(sizeW - l.size.size(), ' ') << l.size << "  "
         << std::string(2 * l.depth, ' ');
      describe(*l.node);
      os << '\n';
    }
    if (lines.empty()) {
      dim();
      os << "  (no members)\n";
      reset();
    }

    for (const VTable &vt : r.vtables) {
      os << '\n';
      vtable(vt);
    }
  }

private:
  llvm::raw_ostream &os;
  bool ms;

  void color(Color c, bool b = false) { os.changeColor(c, b); }
  void bold() { os.changeColor(Color::SAVEDCOLOR, true); }
  void dim() { os.changeColor(Color::BRIGHT_BLACK); }
  void reset() { os.resetColor(); }

  void describe(const Node &n) {
    switch (n.kind) {
    case NodeKind::Record:
      if (n.role == Role::Field) {
        color(Color::CYAN);
        os << n.type;
        reset();
        if (!n.name.empty())
          os << ' ' << n.name;
      } else {
        bold();
        os << n.type;
        reset();
        dim();
        switch (n.role) {
        case Role::PrimaryBase: os << "  primary base"; break;
        case Role::Base: os << "  base"; break;
        case Role::PrimaryVirtualBase: os << "  primary virtual base"; break;
        case Role::VirtualBase: os << "  virtual base"; break;
        default: break;
        }
        reset();
      }
      if (n.empty) {
        dim();
        os << " (empty)";
        reset();
      }
      break;
    case NodeKind::Field:
    case NodeKind::BitField:
      color(Color::CYAN);
      os << n.type;
      reset();
      os << ' ' << (n.name.empty() ? "(unnamed)" : n.name);
      if (n.kind == NodeKind::BitField)
        os << " : " << n.sizeBits;
      break;
    case NodeKind::VPtr:
      color(Color::MAGENTA, true);
      os << (ms ? "vfptr" : "vptr");
      reset();
      dim();
      os << "  " << n.type;
      reset();
      if (n.vtable) {
        color(Color::YELLOW);
        os << "  -> " << n.vtable->table << '[' << n.vtable->slot << ']';
        reset();
      }
      break;
    case NodeKind::VBPtr:
      color(Color::MAGENTA, true);
      os << "vbptr";
      reset();
      dim();
      os << "  " << n.type;
      reset();
      break;
    case NodeKind::VtorDisp:
      color(Color::MAGENTA, true);
      os << "vtordisp";
      reset();
      dim();
      os << "  for " << n.type;
      reset();
      break;
    case NodeKind::Padding:
      color(Color::RED);
      os << "padding";
      reset();
      break;
    }
  }

  void vtable(const VTable &vt) {
    bold();
    os << vt.name;
    reset();
    dim();
    if (vt.vfptrOffset)
      os << "  vfptr at " << *vt.vfptrOffset << ',';
    os << (vt.vfptrOffset ? " " : "  ") << vt.entries.size()
       << (vt.entries.size() == 1 ? " entry" : " entries");
    reset();
    os << '\n';

    size_t idxW = std::to_string(vt.entries.size()).size() + 3;
    // An address point sits between two entries. Print it after the entry
    // before it: a table with no functions has its address point at the
    // index where the next table starts.
    auto points = [&](size_t index) {
      for (const AddressPoint &ap : vt.addressPoints) {
        if (ap.index != index)
          continue;
        os << std::string(idxW + 2, ' ');
        color(Color::YELLOW);
        os << "address point:";
        for (size_t k = 0; k < ap.subobjects.size(); ++k)
          os << (k ? ", " : " ") << ap.subobjects[k].type << " @"
             << ap.subobjects[k].offset;
        reset();
        os << '\n';
      }
    };
    points(0);
    for (size_t i = 0; i < vt.entries.size(); ++i) {
      if (i && std::find(vt.tableStarts.begin(), vt.tableStarts.end(), i) !=
                   vt.tableStarts.end())
        os << '\n';
      std::string idx = std::to_string(i);
      os << std::string(idxW - idx.size(), ' ') << idx << "  ";
      entry(vt.entries[i]);
      os << '\n';
      points(i + 1);
    }
  }

  void entry(const VTableEntry &e) {
    auto offsetEntry = [&](const char *label) {
      dim();
      os << label;
      reset();
      os << ' ' << e.value;
    };
    switch (e.kind) {
    case EntryKind::VCallOffset: offsetEntry("vcall_offset"); break;
    case EntryKind::VBaseOffset: offsetEntry("vbase_offset"); break;
    case EntryKind::OffsetToTop: offsetEntry("offset_to_top"); break;
    case EntryKind::RTTI:
      dim();
      os << "RTTI";
      reset();
      os << ' ' << e.text;
      break;
    default:
      os << e.text;
      dim();
      if (e.kind == EntryKind::CompleteDtor)
        os << " [complete]";
      else if (e.kind == EntryKind::DeletingDtor)
        os << (ms ? " [scalar deleting]" : " [deleting]");
      else if (e.kind == EntryKind::UnusedFunction)
        os << " [unused]";
      reset();
      if (e.pure) {
        color(Color::RED);
        os << " [pure]";
        reset();
      }
    }
    if (e.thunk) {
      const Thunk &t = *e.thunk;
      color(Color::YELLOW);
      os << " [thunk: this " << (t.thisAdjust < 0 ? "-" : "+")
         << (t.thisAdjust < 0 ? -t.thisAdjust : t.thisAdjust);
      if (t.vcallOffsetAt)
        os << ", vcall offset at " << t.vcallOffsetAt;
      else if (t.virtualThis)
        os << ", virtual this adjustment";
      if (t.returnAdjust)
        os << ", return " << (t.returnAdjust < 0 ? "-" : "+")
           << (t.returnAdjust < 0 ? -t.returnAdjust : t.returnAdjust);
      if (t.vbaseOffsetAt)
        os << ", vbase offset at " << t.vbaseOffsetAt;
      else if (t.virtualReturn)
        os << ", virtual return adjustment";
      os << ']';
      reset();
    }
  }
};

} // namespace

void printText(llvm::raw_ostream &os, const Report &report) {
  Printer p(os, report.abi == "microsoft");
  for (size_t i = 0; i < report.records.size(); ++i) {
    if (i)
      os << '\n';
    p.record(report.records[i]);
  }
}

} // namespace lense
