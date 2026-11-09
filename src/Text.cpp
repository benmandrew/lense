#include "Text.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <set>
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

std::string fmtAlign(const Node &n) {
  if (!n.alignBits || n.kind == NodeKind::BitField ||
      n.kind == NodeKind::Padding)
    return "";
  return std::to_string(n.alignBits / 8);
}

std::string fmtSizeOf(const Node &n) {
  bool inBits = n.kind == NodeKind::BitField ||
                (n.kind == NodeKind::Padding &&
                 (n.offsetBits % 8 || n.sizeBits % 8));
  return fmtSize(n.sizeBits, inBits);
}

struct Line {
  std::string offset;
  std::string size;
  std::string align;
  unsigned depth;
  const Node *node;
};

void collect(const Node &n, unsigned depth, std::vector<Line> &out) {
  out.push_back(
      {fmtOffset(n.offsetBits), fmtSizeOf(n), fmtAlign(n), depth, &n});
  if (n.kind == NodeKind::Record)
    for (const Node &c : n.children)
      collect(c, depth + 1, out);
}

std::string pad(const std::string &s, size_t w) {
  return std::string(w > s.size() ? w - s.size() : 0, ' ') + s;
}

class Printer {
public:
  Printer(llvm::raw_ostream &os, const Report &report)
      : os(os), ms(report.abi == "microsoft"), bigEndian(report.bigEndian),
        cacheLine(report.cacheLine) {}

  void record(const Record &r) {
    source(r);
    header(r);
    traits(r);

    std::vector<Line> lines;
    for (const Node &c : r.layout.children)
      collect(c, 0, lines);
    size_t offW = 6, sizeW = 4, alignW = 5;
    for (const Line &l : lines) {
      offW = std::max(offW, l.offset.size());
      sizeW = std::max(sizeW, l.size.size());
      alignW = std::max(alignW, l.align.size());
    }
    dim();
    os << "  " << pad("offset", offW) << "  " << pad("size", sizeW) << "  "
       << pad("align", alignW) << '\n';
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
        os << "  " << std::string(offW + sizeW + alignW + 6, ' ')
           << "--- cache line " << next / line << " (byte " << next / 8
           << ") ---\n";
        reset();
        next += line;
      }
      os << "  " << pad(l.offset, offW) << "  " << pad(l.size, sizeW) << "  "
         << pad(l.align, alignW) << "  " << std::string(2 * l.depth, ' ');
      describe(*l.node);
      notes(*l.node);
      os << '\n';
    }
    if (lines.empty()) {
      dim();
      os << "  (no members)\n";
      reset();
    }
    reorder(r);
  }

  // "struct S  size 16, align 8 from d, padding 7 bytes"
  void header(const Record &r) {
    bold();
    os << r.tag << ' ' << r.name;
    reset();
    dim();
    os << "  size " << r.size << ", align " << r.align;
    if (!r.alignText.empty())
      os << " from " << r.alignText;
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
    os << "  reordering the fields saves " << fmtAmount((r.size - r.reorder->size) * 8)
       << " (size " << r.reorder->size << "):";
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
    if (n.unaligned) {
      color(Color::RED);
      os << "  (unaligned)";
      reset();
    }
    if (n.crossesCacheLine) {
      color(Color::YELLOW);
      os << "  (crosses cache line)";
      reset();
    }
  }

  llvm::raw_ostream &os;
  bool ms;
  bool bigEndian;
  unsigned cacheLine;

  // The definition with line numbers, e.g. "   3 | struct S {".
  void source(const Record &r) {
    if (r.source.empty())
      return;
    llvm::SmallVector<llvm::StringRef, 16> lines;
    llvm::StringRef(r.source).rtrim('\n').split(lines, '\n');
    std::string last = std::to_string(r.sourceLine + lines.size() - 1);
    size_t width = std::max<size_t>(4, last.size());
    size_t at = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
      std::string n = std::to_string(r.sourceLine + i);
      dim();
      os << std::string(width - n.size(), ' ') << n << " |";
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
      switch (s.style) {
      case SourceStyle::Keyword: color(Color::BLUE); break;
      case SourceStyle::Literal: color(Color::GREEN); break;
      case SourceStyle::Comment: dim(); break;
      }
      os << line.slice(b, e);
      reset();
      pos = e;
    }
    os << line.drop_front(pos);
  }

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
  std::map<std::string, unsigned> seen;
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
  std::map<std::string, size_t> inB;
  for (size_t j = 0; j < kb.size(); ++j)
    inB[kb[j]] = j;
  std::vector<bool> used(kb.size());
  std::set<std::string> inA(ka.begin(), ka.end());
  size_t next = 0;
  auto onlyB = [&](size_t upTo) {
    for (; next < upTo; ++next)
      if (!used[next] && !inA.count(kb[next])) {
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
         p.a->sizeBits == p.b->sizeBits && fmtAlign(*p.a) == fmtAlign(*p.b);
}

void compareRecord(Printer &pa, Printer &pb, const Record &a, const Record &b,
                   const std::string &ta, const std::string &tb) {
  llvm::raw_ostream &os = pa.os;
  std::vector<Pair> rows;
  merge(a.layout, b.layout, 0, rows);
  bool same = a.size == b.size && a.align == b.align &&
              std::all_of(rows.begin(), rows.end(), sameRow);

  pa.bold();
  os << a.tag << ' ' << a.name;
  pa.reset();
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
  auto summary = [&](Printer &p, const Record &r, const std::string &t) {
    os << "  " << t << std::string(tw - t.size(), ' ');
    p.dim();
    os << "  size " << r.size << ", align " << r.align;
    if (!r.alignText.empty())
      os << " from " << r.alignText;
    if (r.paddingBits)
      os << ", padding " << fmtAmount(r.paddingBits);
    p.reset();
    os << '\n';
  };
  summary(pa, a, ta);
  summary(pb, b, tb);

  struct Cells {
    std::string offset, size, align;
  };
  auto cells = [](const Node *n) {
    return n ? Cells{fmtOffset(n->offsetBits), fmtSizeOf(*n), fmtAlign(*n)}
             : Cells{};
  };
  size_t offW = 6, sizeW = 4, alignW = 5;
  for (const Pair &p : rows)
    for (const Node *n : {p.a, p.b}) {
      Cells c = cells(n);
      offW = std::max(offW, c.offset.size());
      sizeW = std::max(sizeW, c.size.size());
      alignW = std::max(alignW, c.align.size());
    }
  size_t group = offW + sizeW + alignW + 4;
  size_t gw = std::max(group, tw);
  pa.dim();
  os << "  " << std::string(gw - group, ' ') << pad(ta, group) << "  "
     << std::string(gw - group, ' ') << pad(tb, group) << '\n';
  os << "  ";
  for (int k = 0; k < 2; ++k)
    os << (k ? "  " : "") << std::string(gw - group, ' ')
       << pad("offset", offW) << "  " << pad("size", sizeW) << "  "
       << pad("align", alignW);
  os << '\n';
  pa.reset();
  for (const Pair &p : rows) {
    bool changed = !sameRow(p);
    if (changed)
      pa.color(Color::YELLOW, true);
    os << (changed ? "* " : "  ");
    for (const Node *n : {p.a, p.b}) {
      Cells c = cells(n);
      os << std::string(gw - group, ' ') << pad(c.offset, offW) << "  "
         << pad(c.size, sizeW) << "  " << pad(c.align, alignW) << "  ";
    }
    if (changed)
      pa.reset();
    os << std::string(2 * p.depth, ' ');
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
  std::map<std::string, const Record *> inB;
  for (const Record &r : b.records)
    inB[r.name] = &r;
  std::set<std::string> inA;
  bool first = true;
  auto gap = [&] {
    if (!first)
      os << '\n';
    first = false;
  };
  auto only = [&](Printer &p, const Record &r, const std::string &t) {
    gap();
    p.bold();
    os << r.tag << ' ' << r.name;
    p.reset();
    p.dim();
    os << "  only on " << t << '\n';
    p.reset();
  };
  for (const Record &r : a.records) {
    inA.insert(r.name);
    auto it = inB.find(r.name);
    if (it == inB.end()) {
      only(pa, r, a.target);
      continue;
    }
    gap();
    if (!r.source.empty())
      pa.source(r);
    compareRecord(pa, pb, r, *it->second, a.target, b.target);
  }
  for (const Record &r : b.records)
    if (!inA.count(r.name))
      only(pb, r, b.target);
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
