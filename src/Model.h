// The layout model: a plain description of records and vtables with no
// dependency on clang. Analyze.cpp fills it; Json.cpp and Text.cpp print it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lense {

enum class NodeKind {
  Record,   // A base subobject, a field of class type, or the root record.
  Field,    // A non-bitfield data member of non-class type.
  BitField, // A bitfield data member.
  VPtr,     // Itanium vtable pointer or Microsoft vftable pointer.
  VBPtr,    // Microsoft virtual-base table pointer.
  VtorDisp, // Microsoft vtordisp field in front of a virtual base.
  Padding,  // Bits that no member occupies.
};

enum class Role {
  Root,
  Field,
  PrimaryBase,
  Base,
  PrimaryVirtualBase,
  VirtualBase,
};

// Where a vtable pointer points: entry `slot` of the table called `table`.
struct VTableRef {
  std::string table;
  unsigned slot = 0;
};

struct Node {
  NodeKind kind = NodeKind::Field;
  Role role = Role::Field;
  // Both in bits, measured from the start of the outermost record.
  uint64_t offsetBits = 0;
  uint64_t sizeBits = 0;
  std::string type; // Spelling of the type (class name for records).
  std::string name; // Member name. Empty for bases and unnamed members.
  std::string tag;  // "struct", "class" or "union" for Record nodes.
  bool empty = false;
  // Alignment the member asks for, from its type and any alignas on it.
  // Zero for bitfields and padding.
  uint64_t alignBits = 0;
  // The member is not guaranteed to sit at a multiple of alignBits: its
  // offset, or the alignment of the outermost record, is too small.
  bool unaligned = false;
  // A leaf that spans a cache-line boundary, counting from the start of the
  // outermost record.
  bool crossesCacheLine = false;
  // BitField: the memory location it belongs to, numbered from 1 within the
  // outermost record. Adjacent bitfields of nonzero width share one.
  unsigned location = 0;
  std::optional<VTableRef> vtable; // Set on VPtr nodes when resolvable.
  std::vector<Node> children;
};

struct Hole {
  uint64_t offsetBits = 0;
  uint64_t sizeBits = 0;
};

enum class EntryKind {
  VCallOffset,
  VBaseOffset,
  OffsetToTop,
  RTTI,
  Function,
  CompleteDtor,
  DeletingDtor, // On Microsoft targets this is the scalar deleting destructor.
  UnusedFunction,
};

struct Thunk {
  int64_t thisAdjust = 0;      // Non-virtual this-pointer adjustment.
  int64_t vcallOffsetAt = 0;   // Itanium: offset of the vcall offset to load.
  int64_t returnAdjust = 0;    // Non-virtual return-value adjustment.
  int64_t vbaseOffsetAt = 0;   // Itanium: offset of the vbase offset to load.
  bool virtualThis = false;    // Some virtual this-adjustment (any ABI).
  bool virtualReturn = false;  // Some virtual return adjustment (any ABI).
};

struct VTableEntry {
  EntryKind kind = EntryKind::Function;
  int64_t value = 0;  // Offsets, in bytes.
  std::string text;   // Function signature or RTTI class name.
  bool pure = false;
  std::optional<Thunk> thunk;
};

struct Subobject {
  std::string type;
  int64_t offset = 0; // Bytes from the start of the complete object.
};

struct AddressPoint {
  unsigned index = 0; // Entry the vtable pointer points at.
  std::vector<Subobject> subobjects;
};

struct VTable {
  std::string name;
  std::vector<VTableEntry> entries;
  std::vector<AddressPoint> addressPoints;
  // Itanium: start index of each table in the vtable group.
  std::vector<unsigned> tableStarts;
  // Microsoft: offset of the vfptr that points at this vftable.
  std::optional<int64_t> vfptrOffset;
};

enum class SourceStyle { Keyword, Literal, Comment };

// A highlighted run of Record::source, as byte offsets into it.
struct SourceSpan {
  unsigned begin = 0;
  unsigned end = 0;
  SourceStyle style = SourceStyle::Keyword;
};

// What set a record's alignment.
enum class AlignSource {
  None,      // Nothing beyond the minimum of 1.
  Member,    // The member named in `text` ("y", "base A", "vptr").
  Attribute, // An alignment attribute on the record, spelled in `text`.
  Packed,    // __attribute__((packed)) lowered it.
  Pack,      // #pragma pack or -fpack-struct lowered it; `text` says which.
};

// Type traits that decide what code may do with the bytes. C++ only.
struct Traits {
  bool standardLayout = false;     // offsetof is defined.
  bool triviallyCopyable = false;  // memcpy copies the value.
  bool uniqueObjectRepresentations = false; // Equal values, equal bytes.
};

// A field order with a smaller size, checked by laying it out with clang.
struct Reorder {
  uint64_t size = 0;            // Bytes.
  std::vector<unsigned> order;  // Indices into the fields, in new order.
  std::vector<std::string> fields; // Their names; "" for unnamed ones.
};

struct Record {
  std::string name;
  std::string tag;
  unsigned line = 0;
  unsigned column = 0;
  uint64_t size = 0;      // All sizes and alignments in bytes.
  uint64_t align = 0;
  uint64_t dataSize = 0;
  uint64_t nvSize = 0;
  uint64_t nvAlign = 0;
  uint64_t paddingBits = 0;
  AlignSource alignSource = AlignSource::None;
  std::string alignText;
  std::optional<Traits> traits;
  std::optional<Reorder> reorder;
  bool hasBitFields = false;
  // The definition as written, in whole lines with common indentation
  // removed, and the line it starts on. Empty unless asked for.
  std::string source;
  unsigned sourceLine = 0;
  std::vector<SourceSpan> sourceSpans; // In order, never overlapping.
  Node layout;
  std::vector<Hole> holes;
  std::vector<VTable> vtables;
};

struct Report {
  std::string target;
  std::string abi; // "itanium" or "microsoft".
  unsigned pointerBits = 0;
  bool bigEndian = false;
  unsigned cacheLine = 0; // Bytes; 0 when cache lines are not marked.
  std::vector<Record> records;
};

} // namespace lense
