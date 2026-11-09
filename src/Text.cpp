#include "Text.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <string>

namespace lense {
namespace {

using Color = llvm::raw_ostream::Colors;

constexpr Color Dim = Color::BRIGHT_BLACK;

std::string fmtAmount(uint64_t bits) {
  if (bits % 8 == 0)
    return std::to_string(bits / 8) + (bits == 8 ? " byte" : " bytes");
  return std::to_string(bits) + " bits";
}

// The alignment shown for a member, in bits; 0 leaves the column blank.
uint64_t shownAlign(const Node &n) {
  return n.kind == NodeKind::BitField || n.kind == NodeKind::Padding
             ? 0
             : n.alignBits;
}

// The offset, size and align columns of one row. An offset that is not
// whole bytes is "byte:bit". Sizes of bitfields, and of padding or members
// that are not whole bytes, are in bits with a "b" suffix.
struct Cells {
  std::string offset, size, align;
};

Cells cells(const Node &n) {
  Cells c;
  c.offset = std::to_string(n.offsetBits / 8);
  if (n.offsetBits % 8)
    c.offset += ":" + std::to_string(n.offsetBits % 8);
  bool inBits = n.kind == NodeKind::BitField || n.sizeBits % 8 ||
                (n.kind == NodeKind::Padding && n.offsetBits % 8);
  c.size = inBits ? std::to_string(n.sizeBits) + "b"
                  : std::to_string(n.sizeBits / 8);
  if (uint64_t a = shownAlign(n))
    c.align = std::to_string(a / 8);
  return c;
}

// Column widths, wide enough for the headings and every row.
struct Columns {
  size_t offset = 6, size = 4, align = 5;

  void fit(const Cells &c) {
    offset = std::max(offset, c.offset.size());
    size = std::max(size, c.size.size());
    align = std::max(align, c.align.size());
  }
  size_t width() const { return offset + size + align + 4; }
  void print(llvm::raw_ostream &os, llvm::StringRef o, llvm::StringRef s,
             llvm::StringRef a) const {
    os << llvm::right_justify(o, offset) << "  "
       << llvm::right_justify(s, size) << "  "
       << llvm::right_justify(a, align);
  }
  void print(llvm::raw_ostream &os, const Cells &c) const {
    print(os, c.offset, c.size, c.align);
  }
};

struct Line {
  Cells cells;
  unsigned depth;
  const Node *node;
};

void collect(const Node &n, unsigned depth, std::vector<Line> &out) {
  out.push_back({cells(n), depth, &n});
  if (n.kind == NodeKind::Record)
    for (const Node &c : n.children)
      collect(c, depth + 1, out);
}

struct Printer {
  Printer(llvm::raw_ostream &os, const Report &report)
      : os(os), ms(report.abi == "microsoft"), bigEndian(report.bigEndian),
        cacheLine(report.cacheLine) {}

  llvm::raw_ostream &os;
  bool ms;
  bool bigEndian;
  unsigned cacheLine;

  void color(Color c, bool b = false) { os.changeColor(c, b); }
  void bold() { os.changeColor(Color::SAVEDCOLOR, true); }
  void dim() { os.changeColor(Dim); }
  void reset() { os.resetColor(); }
  void paint(Color c, llvm::StringRef s, bool b = false) {
    color(c, b);
    os << s;
    reset();
  }

  void record(const Record &r) {
    source(r);
    header(r);
    traits(r);

    std::vector<Line> lines;
    for (const Node &c : r.layout.children)
      collect(c, 0, lines);
    Columns cols;
    for (const Line &l : lines)
      cols.fit(l.cells);
    dim();
    os << "  ";
    cols.print(os, "offset", "size", "align");
    os << '\n';
    reset();
    // Rows come in tree order, which is offset order except for members
    // that overlap others, so mark each boundary before the first row
    // that starts at or after it.
    uint64_t line = uint64_t(cacheLine) * 8, next = line;
    for (const Line &l : lines) {
      if (line && l.node->offsetBits >= next) {
        while (l.node->offsetBits >= next + line)
          next += line;
        dim();
        os.indent(cols.width() + 4)
            << "--- cache line " << next / line << " (byte " << next / 8
            << ") ---\n";
        reset();
        next += line;
      }
      os << "  ";
      cols.print(os, l.cells);
      os.indent(2 + 2 * l.depth);
      describe(*l.node);
      notes(*l.node);
      os << '\n';
    }
    if (lines.empty())
      paint(Dim, "  (no members)\n");
    reorder(r);
  }

  void title(const Record &r) {
    bold();
    os << r.tag << ' ' << r.name;
    reset();
  }

  void sizeAlign(const Record &r) {
    os << "  size " << r.size << ", align " << r.align;
    if (!r.alignText.empty())
      os << " from " << r.alignText;
  }

  // "struct S  size 16, align 8 from d, padding 7 bytes"
  void header(const Record &r) {
    title(r);
    dim();
    sizeAlign(r);
    if (r.dataSize != r.size)
      os << ", data size " << r.dataSize;
    if (r.nvSize != r.size && r.nvSize != r.dataSize)
      os << ", non-virtual size " << r.nvSize;
    if (r.nvAlign != r.align)
      os << ", non-virtual align " << r.nvAlign;
    if (r.paddingBits)
      os << ", padding " << fmtAmount(r.paddingBits);
    if (r.hasBitFields)
      os << (bigEndian ? ", big-endian" : ", little-endian");
    reset();
    os << '\n';
  }

  void traits(const Record &r) {
    if (!r.traits)
      return;
    auto yes = [](bool b) { return b ? "yes" : "no"; };
    dim();
    os << "  is_standard_layout " << yes(r.traits->standardLayout)
       << ", is_trivially_copyable " << yes(r.traits->triviallyCopyable)
       << ", has_unique_object_representations "
       << yes(r.traits->uniqueObjectRepresentations) << '\n';
    reset();
  }

  void reorder(const Record &r) {
    if (!r.reorder)
      return;
    color(Color::GREEN);
    os << "  reordering the fields saves "
       << fmtAmount((r.size - r.reorder->size) * 8) << " (size "
       << r.reorder->size << "):";
    for (size_t i = 0; i < r.reorder->fields.size(); ++i)
      os << (i ? ", " : " ")
         << (r.reorder->fields[i].empty() ? "(unnamed)" : r.reorder->fields[i]);
    reset();
    os << '\n';
  }

  void notes(const Node &n) {
    if (n.location) {
      dim();
      os << "  location " << n.location;
      reset();
    }
    if (n.unaligned)
      paint(Color::RED, "  (unaligned)");
    if (n.crossesCacheLine)
      paint(Color::YELLOW, "  (crosses cache line)");
  }

  // The definition with line numbers, e.g. "   3 | struct S {".
  void source(const Record &r) {
    if (r.source.empty())
      return;
    llvm::SmallVector<llvm::StringRef, 16> lines;
    llvm::StringRef(r.source).rtrim('\n').split(lines, '\n');
    unsigned width = std::max<size_t>(
        4, std::to_string(r.sourceLine + lines.size() - 1).size());
    size_t at = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
      dim();
      os << llvm::format_decimal(r.sourceLine + i, width) << " |";
      reset();
      if (!lines[i].empty()) {
        os << ' ';
        sourceText(r, lines[i], at);
      }
      os << '\n';
      at += lines[i].size() + 1;
    }
  }

  // One line of r.source that starts at offset `at`, with its highlights.
  // A span can cross lines (a block comment), so clip it to this one.
  void sourceText(const Record &r, llvm::StringRef line, size_t at) {
    size_t pos = 0;
    for (const SourceSpan &s : r.sourceSpans) {
      if (s.end <= at || s.begin >= at + line.size())
        continue;
      size_t b = std::max<size_t>(s.begin, at) - at;
      size_t e = std::min<size_t>(s.end, at + line.size()) - at;
      os << line.slice(pos, b);
      Color c = s.style == SourceStyle::Keyword   ? Color::BLUE
                : s.style == SourceStyle::Literal ? Color::GREEN
                                                  : Dim;
      paint(c, line.slice(b, e));
      pos = e;
    }
    os << line.drop_front(pos);
  }

  void describe(const Node &n) {
    switch (n.kind) {
    case NodeKind::Record:
      if (n.role == Role::Field) {
        paint(Color::CYAN, n.type);
        if (!n.name.empty())
          os << ' ' << n.name;
      } else {
        paint(Color::SAVEDCOLOR, n.type, true);
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
      if (n.empty)
        paint(Dim, " (empty)");
      break;
    case NodeKind::Field:
    case NodeKind::BitField:
      paint(Color::CYAN, n.type);
      os << ' ' << (n.name.empty() ? "(unnamed)" : n.name);
      if (n.kind == NodeKind::BitField)
        os << " : " << n.sizeBits;
      break;
    case NodeKind::VPtr:
    case NodeKind::VBPtr:
    case NodeKind::VtorDisp:
      paint(Color::MAGENTA,
            n.kind == NodeKind::VBPtr      ? "vbptr"
            : n.kind == NodeKind::VtorDisp ? "vtordisp"
            : ms                           ? "vfptr"
                                           : "vptr",
            true);
      dim();
      os << (n.kind == NodeKind::VtorDisp ? "  for " : "  ") << n.type;
      reset();
      break;
    case NodeKind::Padding:
      paint(Color::RED, "padding");
      break;
    }
  }
};

// -- Comparing two targets -------------------------------------------------

// One row of a comparison: the same member on each target, or on one only.
struct Pair {
  const Node *a = nullptr;
  const Node *b = nullptr;
  unsigned depth = 0;
};

// Identifies a member among its siblings across targets. Padding is keyed
// by the member it follows, so a hole that moves shows as two rows.
std::vector<std::string> keys(const Node &parent) {
  std::vector<std::string> out;
  llvm::StringMap<unsigned> seen;
  std::string prev = "start";
  for (const Node &c : parent.children) {
    std::string k;
    if (c.kind == NodeKind::Padding) {
      k = "padding after " + prev;
    } else {
      k = std::to_string(int(c.kind)) + '|' + std::to_string(int(c.role)) +
          '|' + c.type + '|' + c.name;
      prev = k;
    }
    out.push_back(k + '#' + std::to_string(seen[k]++));
  }
  return out;
}

void side(const Node &n, unsigned depth, bool isA, std::vector<Pair> &out) {
  out.push_back(isA ? Pair{&n, nullptr, depth} : Pair{nullptr, &n, depth});
  if (n.kind == NodeKind::Record)
    for (const Node &c : n.children)
      side(c, depth + 1, isA, out);
}

// Interleaves the children of a and b, in a's order, with members found
// only in b placed after the member they follow there.
void merge(const Node &a, const Node &b, unsigned depth,
           std::vector<Pair> &out) {
  std::vector<std::string> ka = keys(a), kb = keys(b);
  llvm::StringMap<size_t> inB;
  for (size_t j = 0; j < kb.size(); ++j)
    inB[kb[j]] = j;
  std::vector<bool> used(kb.size());
  llvm::StringSet<> inA(llvm::from_range, ka);
  size_t next = 0;
  auto onlyB = [&](size_t upTo) {
    for (; next < upTo; ++next)
      if (!used[next] && !inA.contains(kb[next])) {
        used[next] = true;
        side(b.children[next], depth, false, out);
      }
  };
  for (size_t i = 0; i < ka.size(); ++i) {
    const Node &x = a.children[i];
    auto it = inB.find(ka[i]);
    if (it == inB.end()) {
      side(x, depth, true, out);
      continue;
    }
    size_t j = it->second;
    onlyB(j);
    used[j] = true;
    const Node &y = b.children[j];
    out.push_back({&x, &y, depth});
    if (x.kind == NodeKind::Record && y.kind == NodeKind::Record)
      merge(x, y, depth + 1, out);
    next = std::max(next, j + 1);
  }
  onlyB(kb.size());
}

bool sameRow(const Pair &p) {
  return p.a && p.b && p.a->offsetBits == p.b->offsetBits &&
         p.a->sizeBits == p.b->sizeBits && shownAlign(*p.a) == shownAlign(*p.b);
}

void compareRecord(Printer &pa, Printer &pb, const Record &a, const Record &b,
                   const std::string &ta, const std::string &tb) {
  llvm::raw_ostream &os = pa.os;
  std::vector<Pair> rows;
  merge(a.layout, b.layout, 0, rows);
  bool same = a.size == b.size && a.align == b.align &&
              llvm::all_of(rows, sameRow);

  pa.title(a);
  pa.dim();
  if (same) {
    os << "  same on both targets: size " << a.size << ", align " << a.align
       << '\n';
    pa.reset();
    return;
  }
  pa.reset();
  os << '\n';
  size_t tw = std::max(ta.size(), tb.size());
  for (auto [r, t] : {std::pair(&a, &ta), std::pair(&b, &tb)}) {
    os << "  " << llvm::left_justify(*t, tw);
    pa.dim();
    pa.sizeAlign(*r);
    if (r->paddingBits)
      os << ", padding " << fmtAmount(r->paddingBits);
    pa.reset();
    os << '\n';
  }

  Columns cols;
  std::vector<Cells> cs; // Two per row, blank for a missing side.
  cs.reserve(2 * rows.size());
  for (const Pair &p : rows)
    for (const Node *n : {p.a, p.b}) {
      cs.push_back(n ? cells(*n) : Cells());
      cols.fit(cs.back());
    }
  size_t group = cols.width();
  size_t lead = std::max(group, tw) - group;
  pa.dim();
  os << "  ";
  os.indent(lead) << llvm::right_justify(ta, group) << "  ";
  os.indent(lead) << llvm::right_justify(tb, group) << '\n';
  for (int k = 0; k < 2; ++k) {
    os.indent(2 + lead);
    cols.print(os, "offset", "size", "align");
  }
  os << '\n';
  pa.reset();
  for (size_t i = 0; i < rows.size(); ++i) {
    const Pair &p = rows[i];
    bool changed = !sameRow(p);
    if (changed)
      pa.color(Color::YELLOW, true);
    os << (changed ? "* " : "  ");
    for (size_t k = 2 * i; k < 2 * i + 2; ++k) {
      os.indent(lead);
      cols.print(os, cs[k]);
      os << "  ";
    }
    if (changed)
      pa.reset();
    os.indent(2 * p.depth);
    if (p.a)
      pa.describe(*p.a);
    else
      pb.describe(*p.b);
    os << '\n';
  }
}

} // namespace

void printTextCompare(llvm::raw_ostream &os, const Report &a, const Report &b) {
  Printer pa(os, a), pb(os, b);
  llvm::StringMap<const Record *> inB;
  for (const Record &r : b.records)
    inB[r.name] = &r;
  llvm::StringSet<> inA;
  bool first = true;
  auto gap = [&] {
    if (!first)
      os << '\n';
    first = false;
  };
  auto only = [&](const Record &r, const std::string &t) {
    gap();
    pa.title(r);
    pa.dim();
    os << "  only on " << t << '\n';
    pa.reset();
  };
  for (const Record &r : a.records) {
    inA.insert(r.name);
    auto it = inB.find(r.name);
    if (it == inB.end()) {
      only(r, a.target);
      continue;
    }
    gap();
    pa.source(r);
    compareRecord(pa, pb, r, *it->second, a.target, b.target);
  }
  for (const Record &r : b.records)
    if (!inA.contains(r.name))
      only(r, b.target);
}

void printText(llvm::raw_ostream &os, const Report &report) {
  Printer p(os, report);
  for (size_t i = 0; i < report.records.size(); ++i) {
    if (i)
      os << '\n';
    p.record(report.records[i]);
  }
}

} // namespace lense
