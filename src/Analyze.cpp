// Walks the AST and turns clang's record and vtable layouts into the model.
// The traversal order follows clang's own -fdump-record-layouts.
#include "Analyze.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/VTableBuilder.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "llvm/ADT/DenseSet.h"

#include <algorithm>
#include <map>

using namespace clang;

namespace lense {
namespace {

// The complete object that a subobject belongs to. Vtable pointers are
// resolved against the complete object's vtable.
struct CompleteObject {
  const CXXRecordDecl *decl = nullptr;
  uint64_t offsetBits = 0;
};

class Analyzer {
public:
  Analyzer(ASTContext &C, const AnalyzeOptions &opts)
      : C(C), opts(opts), policy(C.getPrintingPolicy()) {
    policy.AnonymousTagNameStyle =
        llvm::to_underlying(PrintingPolicy::AnonymousTagMode::Plain);
    policy.SuppressTemplateArgsInCXXConstructors = true;
    ms = C.getTargetInfo().getCXXABI().isMicrosoft();
    pointerBits = C.getTargetInfo().getPointerWidth(LangAS::Default);
  }

  Record analyzeRecord(const RecordDecl *RD) {
    const ASTRecordLayout &L = C.getASTRecordLayout(RD);
    Record R;
    R.name = recordName(RD);
    R.tag = RD->getKindName().str();
    PresumedLoc loc = C.getSourceManager().getPresumedLoc(RD->getLocation());
    if (loc.isValid()) {
      R.line = loc.getLine();
      R.column = loc.getColumn();
    }
    R.size = L.getSize().getQuantity();
    R.align = L.getAlignment().getQuantity();
    R.dataSize = L.getDataSize().getQuantity();
    auto *CXX = dyn_cast<CXXRecordDecl>(RD);
    // Only C++ records carry non-virtual layout info.
    R.nvSize = CXX ? L.getNonVirtualSize().getQuantity() : R.size;
    R.nvAlign = CXX ? L.getNonVirtualAlignment().getQuantity() : R.align;

    R.layout.role = Role::Root;
    layoutRecord(R.layout, RD, 0, CompleteObject{CXX, 0}, true);
    R.layout.sizeBits = C.toBits(L.getSize());

    R.holes = findHoles(R.layout);
    for (const Hole &h : R.holes) {
      R.paddingBits += h.sizeBits;
      placeHole(R.layout, h);
    }

    if (opts.vtables && CXX && CXX->isDynamicClass()) {
      if (ms)
        addMicrosoftVTables(R, CXX);
      else
        addItaniumVTable(R, CXX);
    }
    return R;
  }

  bool msABI() const { return ms; }
  unsigned pointerWidth() const { return pointerBits; }

  std::string recordName(const RecordDecl *RD) const {
    if (!RD->getIdentifier())
      if (const TypedefNameDecl *TD = RD->getTypedefNameForAnonDecl())
        return TD->getQualifiedNameAsString();
    std::string s;
    llvm::raw_string_ostream os(s);
    RD->getNameForDiagnostic(os, policy, /*Qualified=*/true);
    return s;
  }

private:
  ASTContext &C;
  const AnalyzeOptions &opts;
  PrintingPolicy policy;
  bool ms = false;
  unsigned pointerBits = 64;

  std::string typeName(QualType T) const { return T.getAsString(policy); }

  static Node leaf(NodeKind kind, uint64_t offsetBits, uint64_t sizeBits) {
    Node n;
    n.kind = kind;
    n.offsetBits = offsetBits;
    n.sizeBits = sizeBits;
    return n;
  }

  // Fills `N` with the layout of `RD` placed at `offsetBits`. Virtual bases are
  // laid out by the complete object, so only include them for one.
  void layoutRecord(Node &N, const RecordDecl *RD, uint64_t offsetBits,
                    CompleteObject complete, bool includeVBases) {
    const ASTRecordLayout &L = C.getASTRecordLayout(RD);
    auto *CXX = dyn_cast<CXXRecordDecl>(RD);
    N.kind = NodeKind::Record;
    N.offsetBits = offsetBits;
    N.tag = RD->getKindName().str();
    if (N.type.empty())
      N.type = recordName(RD);
    N.empty = CXX && CXX->isEmpty();

    if (CXX) {
      const CXXRecordDecl *primary = L.getPrimaryBase();
      if (!ms && CXX->isDynamicClass() && !primary)
        N.children.push_back(vptr(CXX, offsetBits, complete));
      else if (ms && L.hasOwnVFPtr())
        N.children.push_back(vfptr(CXX, offsetBits, complete));

      std::vector<const CXXRecordDecl *> bases;
      for (const CXXBaseSpecifier &B : CXX->bases())
        if (!B.isVirtual())
          bases.push_back(B.getType()->getAsCXXRecordDecl());
      std::stable_sort(bases.begin(), bases.end(),
                       [&](const CXXRecordDecl *a, const CXXRecordDecl *b) {
                         return L.getBaseClassOffset(a) <
                                L.getBaseClassOffset(b);
                       });
      for (const CXXRecordDecl *B : bases) {
        Node child;
        child.role = B == primary ? Role::PrimaryBase : Role::Base;
        layoutRecord(child, B,
                     offsetBits + C.toBits(L.getBaseClassOffset(B)), complete,
                     false);
        // An empty base shares its address with other subobjects.
        child.sizeBits =
            child.empty
                ? 0
                : C.toBits(C.getASTRecordLayout(B).getNonVirtualSize());
        N.children.push_back(std::move(child));
      }

      if (L.hasOwnVBPtr()) {
        Node n = leaf(NodeKind::VBPtr,
                      offsetBits + C.toBits(L.getVBPtrOffset()), pointerBits);
        n.type = recordName(CXX);
        N.children.push_back(std::move(n));
      }
    }

    unsigned fieldNo = 0;
    for (const FieldDecl *F : RD->fields()) {
      uint64_t fieldBits = offsetBits + L.getFieldOffset(fieldNo++);
      QualType T = F->getType();
      if (F->isBitField()) {
        Node n = leaf(NodeKind::BitField, fieldBits, F->getBitWidthValue());
        n.type = typeName(T);
        n.name = F->getName().str();
        N.children.push_back(std::move(n));
        continue;
      }
      const RecordDecl *FRD = T->getAsRecordDecl();
      if (FRD && !opts.expandSystem &&
          C.getSourceManager().isInSystemHeader(FRD->getLocation()))
        FRD = nullptr; // Show as a single opaque field.
      if (FRD) {
        if (const RecordDecl *def = FRD->getDefinition()) {
          Node child;
          child.role = Role::Field;
          child.name = F->getName().str();
          child.type = typeName(T);
          if (!def->getIdentifier() && !def->getTypedefNameForAnonDecl())
            child.type = std::string(child.name.empty() ? "(anonymous "
                                                        : "(unnamed ") +
                         def->getKindName().str() + ")";
          layoutRecord(child, def, fieldBits,
                       CompleteObject{dyn_cast<CXXRecordDecl>(def), fieldBits},
                       true);
          // A [[no_unique_address]] member may share its tail padding with
          // later members, so it only owns its data size (zero if empty).
          if (F->isZeroSize(C))
            child.sizeBits = 0;
          else if (F->isPotentiallyOverlapping())
            child.sizeBits = C.toBits(C.getASTRecordLayout(def).getDataSize());
          else
            child.sizeBits = C.toBits(C.getTypeSizeInChars(T));
          N.children.push_back(std::move(child));
          continue;
        }
      }
      Node n = leaf(NodeKind::Field, fieldBits,
                    T->isIncompleteType() ? 0 : C.getTypeSize(T));
      n.type = typeName(T);
      n.name = F->getName().str();
      N.children.push_back(std::move(n));
    }

    if (CXX && includeVBases) {
      const auto &vbaseInfo = L.getVBaseOffsetsMap();
      for (const CXXBaseSpecifier &B : CXX->vbases()) {
        const CXXRecordDecl *VB = B.getType()->getAsCXXRecordDecl();
        uint64_t vbBits = offsetBits + C.toBits(L.getVBaseClassOffset(VB));
        auto it = vbaseInfo.find(VB);
        if (it != vbaseInfo.end() && it->second.hasVtorDisp()) {
          Node n = leaf(NodeKind::VtorDisp, vbBits - 32, 32);
          n.type = recordName(VB);
          N.children.push_back(std::move(n));
        }
        Node child;
        child.role = VB == L.getPrimaryBase() ? Role::PrimaryVirtualBase
                                              : Role::VirtualBase;
        layoutRecord(child, VB, vbBits, complete, false);
        child.sizeBits =
            C.toBits(C.getASTRecordLayout(VB).getNonVirtualSize());
        N.children.push_back(std::move(child));
      }
    }
  }

  // Itanium: a class with no primary base owns a vtable pointer. It points at
  // the address point of this subobject in the complete object's vtable.
  Node vptr(const CXXRecordDecl *owner, uint64_t offsetBits,
            CompleteObject complete) {
    Node n = leaf(NodeKind::VPtr, offsetBits, pointerBits);
    n.type = recordName(owner);
    if (!complete.decl || !opts.vtables)
      return n;
    auto *ctx = dyn_cast<ItaniumVTableContext>(C.getVTableContext());
    if (!ctx)
      return n;
    const VTableLayout &VL = ctx->getVTableLayout(complete.decl);
    BaseSubobject key(owner,
                      C.toCharUnitsFromBits(offsetBits - complete.offsetBits));
    auto it = VL.getAddressPoints().find(key);
    if (it != VL.getAddressPoints().end())
      n.vtable = VTableRef{"vtable for " + recordName(complete.decl),
                           unsigned(VL.getVTableOffset(it->second.VTableIndex) +
                                    it->second.AddressPointIndex)};
    return n;
  }

  // Microsoft: each vfptr points at its own vftable, found by its offset in
  // the complete object.
  Node vfptr(const CXXRecordDecl *owner, uint64_t offsetBits,
             CompleteObject complete) {
    Node n = leaf(NodeKind::VPtr, offsetBits, pointerBits);
    n.type = recordName(owner);
    if (!complete.decl || !opts.vtables)
      return n;
    auto *ctx = dyn_cast<MicrosoftVTableContext>(C.getVTableContext());
    if (!ctx)
      return n;
    CharUnits rel = C.toCharUnitsFromBits(offsetBits - complete.offsetBits);
    for (const auto &info : ctx->getVFPtrOffsets(complete.decl)) {
      if (info->FullOffsetInMDC != rel)
        continue;
      const VTableLayout &VL =
          ctx->getVFTableLayout(complete.decl, info->FullOffsetInMDC);
      unsigned slot = 0;
      auto comps = VL.vtable_components();
      while (slot < comps.size() &&
             comps[slot].getKind() == VTableComponent::CK_RTTI)
        ++slot;
      n.vtable = VTableRef{vftableName(complete.decl, *info), slot};
    }
    return n;
  }

  std::string vftableName(const CXXRecordDecl *RD, const VPtrInfo &info) {
    std::string s = "vftable for ";
    // Same naming as clang's -fdump-vtable-layouts.
    for (const CXXRecordDecl *B : llvm::reverse(info.PathToIntroducingObject))
      s += recordName(B) + " in ";
    return s + recordName(RD);
  }

  // Collects every occupied bit range and returns the gaps between them.
  static std::vector<Hole> findHoles(const Node &root) {
    std::vector<std::pair<uint64_t, uint64_t>> used;
    std::vector<const Node *> stack{&root};
    while (!stack.empty()) {
      const Node *n = stack.back();
      stack.pop_back();
      if (n->kind == NodeKind::Record && !n->empty) {
        for (const Node &c : n->children)
          stack.push_back(&c);
      } else if (n->sizeBits && n != &root) {
        used.emplace_back(n->offsetBits, n->offsetBits + n->sizeBits);
      }
    }
    std::sort(used.begin(), used.end());
    std::vector<Hole> holes;
    uint64_t pos = 0;
    for (auto [begin, end] : used) {
      if (begin > pos)
        holes.push_back({pos, begin - pos});
      pos = std::max(pos, end);
    }
    if (root.sizeBits > pos)
      holes.push_back({pos, root.sizeBits - pos});
    return holes;
  }

  // Puts a hole in the innermost non-empty record that contains all of it.
  static void placeHole(Node &N, const Hole &h) {
    for (Node &c : N.children) {
      if (c.kind == NodeKind::Record && !c.empty &&
          c.offsetBits <= h.offsetBits &&
          h.offsetBits + h.sizeBits <= c.offsetBits + c.sizeBits) {
        placeHole(c, h);
        return;
      }
    }
    Node pad = leaf(NodeKind::Padding, h.offsetBits, h.sizeBits);
    auto pos = std::upper_bound(
        N.children.begin(), N.children.end(), h.offsetBits,
        [](uint64_t off, const Node &c) { return off < c.offsetBits; });
    N.children.insert(pos, std::move(pad));
  }

  // __PRETTY_FUNCTION__ spelling, minus the " [T = int]" suffix it adds
  // for members of templates; the class name already shows the arguments.
  static std::string functionName(const CXXMethodDecl *MD) {
    std::string s = PredefinedExpr::ComputeName(
        PredefinedIdentKind::PrettyFunctionNoVirtual, MD);
    size_t paren = s.rfind(')');
    size_t bracket = s.rfind(" [");
    if (!s.empty() && s.back() == ']' && bracket != std::string::npos &&
        (paren == std::string::npos || bracket > paren))
      s.erase(bracket);
    return s;
  }

  VTableEntry entry(const VTableComponent &comp) {
    VTableEntry e;
    switch (comp.getKind()) {
    case VTableComponent::CK_VCallOffset:
      e.kind = EntryKind::VCallOffset;
      e.value = comp.getVCallOffset().getQuantity();
      break;
    case VTableComponent::CK_VBaseOffset:
      e.kind = EntryKind::VBaseOffset;
      e.value = comp.getVBaseOffset().getQuantity();
      break;
    case VTableComponent::CK_OffsetToTop:
      e.kind = EntryKind::OffsetToTop;
      e.value = comp.getOffsetToTop().getQuantity();
      break;
    case VTableComponent::CK_RTTI:
      e.kind = EntryKind::RTTI;
      e.text = recordName(comp.getRTTIDecl());
      break;
    case VTableComponent::CK_FunctionPointer:
    case VTableComponent::CK_CompleteDtorPointer:
    case VTableComponent::CK_DeletingDtorPointer:
    case VTableComponent::CK_UnusedFunctionPointer: {
      auto k = comp.getKind();
      e.kind = k == VTableComponent::CK_FunctionPointer ? EntryKind::Function
               : k == VTableComponent::CK_CompleteDtorPointer
                   ? EntryKind::CompleteDtor
               : k == VTableComponent::CK_DeletingDtorPointer
                   ? EntryKind::DeletingDtor
                   : EntryKind::UnusedFunction;
      const CXXMethodDecl *MD = k == VTableComponent::CK_UnusedFunctionPointer
                                    ? comp.getUnusedFunctionDecl()
                                    : comp.getFunctionDecl();
      e.text = functionName(MD);
      e.pure = MD->isPureVirtual();
      break;
    }
    }
    return e;
  }

  std::vector<VTableEntry> entries(const VTableLayout &VL) {
    std::vector<VTableEntry> out;
    for (const VTableComponent &comp : VL.vtable_components())
      out.push_back(entry(comp));
    for (const auto &[index, info] : VL.vtable_thunks()) {
      if (index >= out.size())
        continue;
      Thunk t;
      t.thisAdjust = info.This.NonVirtual;
      t.returnAdjust = info.Return.NonVirtual;
      t.virtualThis = !info.This.Virtual.isEmpty();
      t.virtualReturn = !info.Return.Virtual.isEmpty();
      if (!ms) {
        t.vcallOffsetAt = info.This.Virtual.Itanium.VCallOffsetOffset;
        t.vbaseOffsetAt = info.Return.Virtual.Itanium.VBaseOffsetOffset;
      }
      out[index].thunk = t;
    }
    return out;
  }

  void addItaniumVTable(Record &R, const CXXRecordDecl *RD) {
    auto *ctx = dyn_cast<ItaniumVTableContext>(C.getVTableContext());
    if (!ctx)
      return;
    const VTableLayout &VL = ctx->getVTableLayout(RD);
    VTable vt;
    vt.name = "vtable for " + recordName(RD);
    vt.entries = entries(VL);
    for (size_t i = 0; i < VL.getNumVTables(); ++i)
      vt.tableStarts.push_back(VL.getVTableOffset(i));

    // The address point map is a DenseMap, so sort for stable output.
    std::map<unsigned, std::vector<Subobject>> points;
    for (const auto &[base, loc] : VL.getAddressPoints())
      points[VL.getVTableOffset(loc.VTableIndex) + loc.AddressPointIndex]
          .push_back({recordName(base.getBase()),
                      base.getBaseOffset().getQuantity()});
    for (auto &[index, subs] : points) {
      std::sort(subs.begin(), subs.end(),
                [](const Subobject &a, const Subobject &b) {
                  return std::tie(a.offset, a.type) <
                         std::tie(b.offset, b.type);
                });
      vt.addressPoints.push_back({index, std::move(subs)});
    }
    R.vtables.push_back(std::move(vt));
  }

  void addMicrosoftVTables(Record &R, const CXXRecordDecl *RD) {
    auto *ctx = dyn_cast<MicrosoftVTableContext>(C.getVTableContext());
    if (!ctx)
      return;
    for (const auto &info : ctx->getVFPtrOffsets(RD)) {
      const VTableLayout &VL =
          ctx->getVFTableLayout(RD, info->FullOffsetInMDC);
      VTable vt;
      vt.name = vftableName(RD, *info);
      vt.entries = entries(VL);
      vt.vfptrOffset = info->FullOffsetInMDC.getQuantity();
      unsigned slot = 0;
      while (slot < vt.entries.size() &&
             vt.entries[slot].kind == EntryKind::RTTI)
        ++slot;
      vt.addressPoints.push_back(
          {slot, {{recordName(RD), *vt.vfptrOffset}}});
      R.vtables.push_back(std::move(vt));
    }
  }
};

class Collector : public RecursiveASTVisitor<Collector> {
public:
  explicit Collector(ASTContext &C) : C(C) {}

  bool shouldVisitTemplateInstantiations() const { return true; }

  bool VisitRecordDecl(RecordDecl *RD) {
    if (RD->isImplicit() || RD->isInvalidDecl() ||
        !RD->isThisDeclarationADefinition() || !RD->isCompleteDefinition() ||
        RD->isDependentType())
      return true;
    if (auto *CXX = dyn_cast<CXXRecordDecl>(RD); CXX && CXX->isLambda())
      return true;
    // Anonymous structs and unions appear inside their parent's layout.
    if (!RD->getIdentifier() && !RD->getTypedefNameForAnonDecl())
      return true;
    if (!C.getSourceManager().isInMainFile(RD->getLocation()))
      return true;
    if (seen.insert(RD).second)
      found.push_back(RD);
    return true;
  }

  std::vector<const RecordDecl *> found;

private:
  ASTContext &C;
  llvm::DenseSet<const RecordDecl *> seen;
};

} // namespace

Report analyze(ASTContext &C, const AnalyzeOptions &opts) {
  Analyzer A(C, opts);
  Report report;
  report.target = C.getTargetInfo().getTriple().str();
  report.abi = A.msABI() ? "microsoft" : "itanium";
  report.pointerBits = A.pointerWidth();

  Collector collector(C);
  collector.TraverseDecl(C.getTranslationUnitDecl());
  for (const RecordDecl *RD : collector.found) {
    if (!opts.records.empty()) {
      std::string name = A.recordName(RD);
      if (std::find(opts.records.begin(), opts.records.end(), name) ==
          opts.records.end())
        continue;
    }
    report.records.push_back(A.analyzeRecord(RD));
  }
  return report;
}

} // namespace lense
