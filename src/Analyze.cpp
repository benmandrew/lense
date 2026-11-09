// Walks the AST and turns clang's record and vtable layouts into the model.
// The traversal order follows clang's own -fdump-record-layouts.
#include "Analyze.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <numeric>

using namespace clang;

namespace lense {
namespace {

class Analyzer {
public:
  Analyzer(ASTContext &C, const AnalyzeOptions &opts)
      : C(C), opts(opts), policy(C.getPrintingPolicy()) {
    policy.AnonymousTagNameStyle =
        llvm::to_underlying(PrintingPolicy::AnonymousTagMode::Plain);
    policy.SuppressTemplateArgsInCXXConstructors = true;
    ms = C.getTargetInfo().getCXXABI().isMicrosoft();
    pointerBits = C.getTargetInfo().getPointerWidth(LangAS::Default);
    pointerAlign = C.getTargetInfo().getPointerAlign(LangAS::Default);
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
    if (opts.source)
      addSource(R, RD);

    R.layout.role = Role::Root;
    layoutRecord(R.layout, RD, 0, true);
    R.layout.sizeBits = C.toBits(L.getSize());
    R.layout.alignBits = C.toBits(L.getAlignment());
    alignSource(R, RD);
    unsigned location = 0;
    annotate(R.layout, R.layout.alignBits, location);
    R.hasBitFields = location > 0;
    if (C.getLangOpts().CPlusPlus)
      R.traits = traits(RD);

    // Padding goes in after the bitfields are numbered: a gap between two
    // adjacent bitfields does not split their memory location.
    R.holes = findHoles(R.layout);
    for (const Hole &h : R.holes) {
      R.paddingBits += h.sizeBits;
      placeHole(R.layout, h);
    }
    if (opts.reorder)
      R.reorder = findReorder(RD, R);
    return R;
  }

  // Copies the definition of RD as written. An instantiation shows its
  // template, and an anonymous struct named by a typedef shows the typedef.
  void addSource(Record &R, const RecordDecl *RD) const {
    const Decl *D = RD;
    if (const TypedefNameDecl *TD = RD->getTypedefNameForAnonDecl();
        TD && !RD->getIdentifier())
      D = TD;
    if (const auto *CXX = dyn_cast<CXXRecordDecl>(RD))
      if (const CXXRecordDecl *P = CXX->getTemplateInstantiationPattern()) {
        D = P;
        if (const ClassTemplateDecl *T = P->getDescribedClassTemplate())
          D = T;
      }

    const SourceManager &SM = C.getSourceManager();
    CharSourceRange range = SM.getExpansionRange(D->getSourceRange());
    SourceLocation end = range.getEnd();
    if (range.isTokenRange())
      end = Lexer::getLocForEndOfToken(end, 0, SM, C.getLangOpts());
    auto [fid, begin] = SM.getDecomposedLoc(range.getBegin());
    auto [endFid, endOff] = SM.getDecomposedLoc(end);
    if (fid.isInvalid() || fid != endFid || endOff < begin)
      return;
    bool invalid = false;
    llvm::StringRef buf = SM.getBufferData(fid, &invalid);
    if (invalid || endOff > buf.size())
      return;

    // Take whole lines, so nested and local classes keep their shape. When
    // the definition starts mid-line, pad its first line to its column.
    size_t lineStart = buf.rfind('\n', begin);
    lineStart = lineStart == llvm::StringRef::npos ? 0 : lineStart + 1;
    llvm::StringRef before = buf.slice(lineStart, begin);
    std::string text = before.trim(" \t").empty()
                           ? before.str()
                           : std::string(begin - lineStart, ' ');
    // Keep the semicolon that closes the definition.
    size_t stop = endOff;
    while (stop < buf.size() && (buf[stop] == ' ' || buf[stop] == '\t'))
      ++stop;
    if (stop < buf.size() && buf[stop] == ';')
      endOff = stop + 1;
    text += buf.slice(begin, endOff).str();

    llvm::SmallVector<llvm::StringRef, 16> lines;
    llvm::StringRef(text).split(lines, '\n');
    size_t indent = llvm::StringRef::npos;
    for (llvm::StringRef l : lines) {
      llvm::StringRef body = l.rtrim(" \t\r");
      if (!body.empty())
        indent = std::min(indent, body.find_first_not_of(" \t"));
    }
    if (indent == llvm::StringRef::npos)
      indent = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
      llvm::StringRef l = lines[i].rtrim(" \t\r");
      R.source += l.size() > indent ? l.drop_front(indent).str() : "";
      R.source += '\n';
    }
    R.sourceLine = SM.getPresumedLoc(range.getBegin()).getLine();
    highlight(R);
  }

  // Marks the keywords, literals and comments in R.source. The text is
  // lexed again as its own buffer, because dedenting moved every offset.
  void highlight(Record &R) const {
    SourceManager &SM = C.getSourceManager();
    FileID fid = SM.createFileID(
        llvm::MemoryBuffer::getMemBufferCopy(R.source, "<source>"));
    const LangOptions &LO = C.getLangOpts();
    Lexer L(fid, SM.getBufferOrFake(fid), SM, LO);
    L.SetCommentRetentionState(true);
    Token tok;
    for (L.LexFromRawLexer(tok); tok.isNot(tok::eof); L.LexFromRawLexer(tok)) {
      SourceStyle style;
      if (tok.is(tok::comment)) {
        style = SourceStyle::Comment;
      } else if (tok::isLiteral(tok.getKind())) {
        style = SourceStyle::Literal;
      } else if (tok.is(tok::raw_identifier)) {
        // The preprocessor put every keyword in the table before parsing.
        auto it = C.Idents.find(tok.getRawIdentifier());
        if (it == C.Idents.end() || !it->getValue()->isKeyword(LO))
          continue;
        style = SourceStyle::Keyword;
      } else {
        continue;
      }
      unsigned begin = SM.getFileOffset(tok.getLocation());
      R.sourceSpans.push_back({begin, begin + tok.getLength(), style});
    }
  }

  // Works out what set the record's alignment, from its direct members.
  void alignSource(Record &R, const RecordDecl *RD) const {
    uint64_t align = R.layout.alignBits;
    if (unsigned attr = RD->getMaxAlignment(); attr && attr == align) {
      for (const AlignedAttr *A : RD->specific_attrs<AlignedAttr>()) {
        if (A->isAlignmentDependent() ||
            A->getAlignment(const_cast<ASTContext &>(C)) != attr)
          continue;
        std::string n = std::to_string(attr / 8);
        R.alignSource = AlignSource::Attribute;
        R.alignText = A->isC11()       ? "_Alignas(" + n + ")"
                      : A->isAlignas() ? "alignas(" + n + ")"
                      : A->isDeclspec() ? "__declspec(align(" + n + "))"
                                        : "__attribute__((aligned(" + n + ")))";
        return;
      }
    }
    uint64_t natural = 0;
    for (const Node &c : R.layout.children)
      natural = std::max(natural, c.alignBits);
    if (align < natural) {
      if (RD->hasAttr<PackedAttr>()) {
        R.alignSource = AlignSource::Packed;
        R.alignText = "__attribute__((packed))";
      } else if (const auto *M = RD->getAttr<MaxFieldAlignmentAttr>()) {
        std::string n = std::to_string(M->getAlignment() / 8);
        R.alignSource = AlignSource::Pack;
        R.alignText = C.getLangOpts().PackStruct * 8 == M->getAlignment()
                          ? "-fpack-struct=" + n
                          : "#pragma pack(" + n + ")";
      }
      return;
    }
    if (align <= 8)
      return;
    for (const Node &c : R.layout.children) {
      if (c.alignBits != align)
        continue;
      R.alignSource = AlignSource::Member;
      switch (c.kind) {
      case NodeKind::VPtr: R.alignText = ms ? "vfptr" : "vptr"; break;
      case NodeKind::VBPtr: R.alignText = "vbptr"; break;
      case NodeKind::VtorDisp: R.alignText = "vtordisp"; break;
      case NodeKind::Record:
        if (c.role == Role::Field) {
          R.alignText = c.name.empty() ? c.type : c.name;
          break;
        }
        R.alignText = (c.role == Role::VirtualBase ||
                               c.role == Role::PrimaryVirtualBase
                           ? "virtual base "
                           : "base ") +
                      c.type;
        break;
      default:
        R.alignText = c.name.empty() ? c.type : c.name;
      }
      return;
    }
  }

  // Marks unaligned members and those that cross a cache line, and numbers
  // the memory locations of bitfields. A memory location is a maximal run of
  // adjacent bitfields of nonzero width ([intro.memory]).
  void annotate(Node &N, uint64_t rootAlign, unsigned &location) const {
    uint64_t line = uint64_t(opts.cacheLine) * 8;
    bool run = false;
    for (Node &c : N.children) {
      if (c.kind == NodeKind::BitField && c.sizeBits &&
          N.tag != "union") {
        if (!run)
          ++location;
        c.location = location;
        run = true;
      } else if (c.kind == NodeKind::BitField && c.sizeBits) {
        c.location = ++location;
        run = false;
      } else {
        run = false;
      }
      if (c.kind != NodeKind::BitField && c.alignBits > 8 && c.sizeBits) {
        uint64_t guaranteed =
            c.offsetBits ? std::min(c.offsetBits & -c.offsetBits, rootAlign)
                         : rootAlign;
        c.unaligned = guaranteed < c.alignBits;
      }
      if (c.kind == NodeKind::Record) {
        annotate(c, rootAlign, location);
      } else if (line && c.sizeBits &&
                 c.offsetBits / line != (c.offsetBits + c.sizeBits - 1) / line) {
        c.crossesCacheLine = true;
      }
    }
  }

  Traits traits(const RecordDecl *RD) const {
    QualType T = C.getCanonicalTagType(RD);
    Traits t;
    const auto *CXX = dyn_cast<CXXRecordDecl>(RD);
    t.standardLayout = !CXX || CXX->isStandardLayout();
    t.triviallyCopyable = T.isTriviallyCopyableType(C);
    t.uniqueObjectRepresentations = C.hasUniqueObjectRepresentations(T);
    return t;
  }

  // The alignment a field asks for: its type's, raised by alignas on it.
  uint64_t fieldAlign(const FieldDecl *F) const {
    QualType T = F->getType();
    if (T->isIncompleteArrayType())
      T = C.getBaseElementType(T);
    uint64_t a = T->isIncompleteType() ? 8 : C.getTypeAlign(T);
    return std::max<uint64_t>(a, F->getMaxAlignment());
  }

  // -- Field reordering ----------------------------------------------------

  // A run of fields that moves as one: a single field, or adjacent
  // bitfields, which would pack differently if split.
  struct Unit {
    std::vector<const FieldDecl *> fields;
    std::vector<unsigned> indices;
    uint64_t sizeBits = 0;
    uint64_t alignBits = 8;
  };

  // Estimates the size of RD with its fields in `order`, starting at `start`
  // bits. Clang checks the winner, so this only has to rank orders.
  static uint64_t estimate(const std::vector<Unit> &units,
                           const std::vector<unsigned> &order, uint64_t start,
                           uint64_t align) {
    uint64_t pos = start;
    for (unsigned u : order) {
      const Unit &x = units[u];
      pos = llvm::alignTo(pos, x.alignBits) + x.sizeBits;
    }
    return llvm::alignTo(std::max<uint64_t>(pos, 8), align);
  }

  // A field's name, or "(anonymous struct)" for an anonymous member.
  static std::string fieldLabel(const FieldDecl *F) {
    if (F->isAnonymousStructOrUnion())
      return std::string("(anonymous ") +
             F->getType()->getAsRecordDecl()->getKindName().str() + ")";
    return F->getName().str();
  }

  static unsigned inversions(const std::vector<unsigned> &order) {
    unsigned n = 0;
    for (size_t i = 0; i < order.size(); ++i)
      for (size_t j = i + 1; j < order.size(); ++j)
        n += order[i] > order[j];
    return n;
  }

  std::optional<Reorder> findReorder(const RecordDecl *RD, const Record &R) {
    const auto *CXX = dyn_cast<CXXRecordDecl>(RD);
    if (RD->isUnion() || R.paddingBits < 8 || (CXX && CXX->getNumVBases()))
      return std::nullopt;
    const ASTRecordLayout &L = C.getASTRecordLayout(RD);

    // Fields may start after the vptr and the data of the bases.
    uint64_t start = 0;
    for (const Node &c : R.layout.children)
      if (c.kind == NodeKind::VPtr)
        start = std::max(start, c.offsetBits + c.sizeBits);
    if (CXX)
      for (const CXXBaseSpecifier &S : CXX->bases()) {
        const CXXRecordDecl *B = S.getType()->getAsCXXRecordDecl();
        if (B->isEmpty())
          continue;
        const ASTRecordLayout &BL = C.getASTRecordLayout(B);
        start = std::max<uint64_t>(start, C.toBits(L.getBaseClassOffset(B)) +
                                    C.toBits(ms ? BL.getNonVirtualSize()
                                                : BL.getDataSize()));
      }

    uint64_t maxField = RD->hasAttr<PackedAttr>() ? 8 : 0;
    if (const auto *M = RD->getAttr<MaxFieldAlignmentAttr>())
      maxField = M->getAlignment();
    std::vector<Unit> units;
    const FieldDecl *flexible = nullptr;
    unsigned index = 0;
    bool inRun = false;
    for (const FieldDecl *F : RD->fields()) {
      unsigned i = index++;
      QualType T = F->getType();
      if (T->isIncompleteArrayType()) {
        flexible = F;
        continue;
      }
      uint64_t a = fieldAlign(F);
      if (RD->hasAttr<PackedAttr>() || F->hasAttr<PackedAttr>())
        a = std::max<uint64_t>(8, F->getMaxAlignment());
      if (maxField)
        a = std::min(a, maxField);
      if (F->isBitField()) {
        if (!inRun)
          units.emplace_back();
        Unit &u = units.back();
        u.fields.push_back(F);
        u.indices.push_back(i);
        u.alignBits = std::max(u.alignBits, a);
        uint64_t first = L.getFieldOffset(u.fields.front()->getFieldIndex());
        u.sizeBits = L.getFieldOffset(F->getFieldIndex()) +
                     F->getBitWidthValue() - first;
        inRun = true;
        continue;
      }
      inRun = false;
      Unit u;
      u.fields.push_back(F);
      u.indices.push_back(i);
      u.alignBits = std::max<uint64_t>(a, 8);
      if (F->isZeroSize(C))
        u.sizeBits = 0;
      else if (const RecordDecl *FR = T->getAsRecordDecl();
               FR && F->isPotentiallyOverlapping())
        u.sizeBits = C.toBits(C.getASTRecordLayout(FR).getDataSize());
      else
        u.sizeBits = C.getTypeSize(T);
      units.push_back(std::move(u));
    }
    if (units.size() < 2)
      return std::nullopt;
    for (Unit &u : units)
      u.sizeBits = llvm::alignTo(u.sizeBits, 8);

    uint64_t align = C.toBits(L.getAlignment());
    std::vector<unsigned> original(units.size());
    std::iota(original.begin(), original.end(), 0);
    uint64_t baseline = estimate(units, original, start, align);

    // Try every order of up to 8 units, else two heuristics: largest
    // alignment first, and greedily filling the gap at each step.
    std::vector<std::vector<unsigned>> candidates;
    if (units.size() <= 8) {
      std::vector<unsigned> best = original, perm = original;
      uint64_t bestSize = baseline;
      unsigned bestInv = 0;
      do {
        uint64_t s = estimate(units, perm, start, align);
        unsigned inv = s <= bestSize ? inversions(perm) : 0;
        if (s < bestSize || (s == bestSize && inv < bestInv)) {
          best = perm;
          bestSize = s;
          bestInv = inv;
        }
      } while (std::next_permutation(perm.begin(), perm.end()));
      candidates.push_back(best);
    }
    std::vector<unsigned> sorted = original;
    std::stable_sort(sorted.begin(), sorted.end(), [&](unsigned a, unsigned b) {
      return units[a].alignBits > units[b].alignBits;
    });
    candidates.push_back(sorted);
    std::vector<unsigned> greedy, left = original;
    uint64_t pos = start;
    while (!left.empty()) {
      auto pick = std::min_element(left.begin(), left.end(), [&](unsigned a,
                                                                 unsigned b) {
        uint64_t pa = llvm::alignTo(pos, units[a].alignBits) - pos;
        uint64_t pb = llvm::alignTo(pos, units[b].alignBits) - pos;
        return pa != pb ? pa < pb : units[a].alignBits > units[b].alignBits;
      });
      pos = llvm::alignTo(pos, units[*pick].alignBits) + units[*pick].sizeBits;
      greedy.push_back(*pick);
      left.erase(pick);
    }
    candidates.push_back(greedy);

    std::optional<Reorder> best;
    for (const std::vector<unsigned> &order : candidates) {
      if (order == original ||
          estimate(units, order, start, align) >= baseline)
        continue;
      std::vector<const FieldDecl *> fields;
      Reorder r;
      for (unsigned u : order)
        for (size_t k = 0; k < units[u].fields.size(); ++k) {
          fields.push_back(units[u].fields[k]);
          r.order.push_back(units[u].indices[k]);
          r.fields.push_back(fieldLabel(units[u].fields[k]));
        }
      if (flexible) {
        fields.push_back(flexible);
        r.order.push_back(flexible->getFieldIndex());
        r.fields.push_back(flexible->getName().str());
      }
      std::optional<uint64_t> size = sizeWithFields(RD, fields);
      if (!size || *size >= R.size || (best && *size >= best->size))
        continue;
      r.size = *size;
      best = std::move(r);
    }
    return best;
  }

  // The size clang gives a copy of RD whose fields are `fields`, in order.
  // The copy keeps RD's bases, attributes and own vptr, but no other
  // members; RD has no virtual bases, so nothing else affects the layout.
  std::optional<uint64_t>
  sizeWithFields(const RecordDecl *RD, llvm::ArrayRef<const FieldDecl *> fields) {
    ASTContext &Ctx = const_cast<ASTContext &>(C);
    DeclContext *DC = const_cast<DeclContext *>(RD->getDeclContext());
    SourceLocation loc = RD->getLocation();
    IdentifierInfo *id = RD->getIdentifier();
    const auto *CXX = dyn_cast<CXXRecordDecl>(RD);
    RecordDecl *N;
    if (CXX) {
      auto *NC = CXXRecordDecl::Create(Ctx, RD->getTagKind(), DC, loc, loc, id);
      NC->startDefinition();
      std::vector<const CXXBaseSpecifier *> bases;
      for (const CXXBaseSpecifier &B : CXX->bases())
        bases.push_back(&B);
      NC->setBases(bases.data(), bases.size());
      bool ownVPtr = ms ? C.getASTRecordLayout(RD).hasOwnVFPtr()
                        : CXX->isPolymorphic() && !NC->isPolymorphic();
      if (ownVPtr) {
        QualType FT = Ctx.getFunctionType(Ctx.VoidTy, {}, {});
        auto *MD = CXXMethodDecl::Create(
            Ctx, NC, loc,
            DeclarationNameInfo(&Ctx.Idents.get("__lense_virtual"), loc), FT,
            Ctx.getTrivialTypeSourceInfo(FT), SC_None, false, false,
            ConstexprSpecKind::Unspecified, loc);
        MD->setVirtualAsWritten(true);
        MD->setAccess(AS_public);
        NC->addDecl(MD);
      }
      N = NC;
    } else {
      N = RecordDecl::Create(Ctx, RD->getTagKind(), DC, loc, loc, id);
      N->startDefinition();
    }
    N->setImplicit(true);
    for (const Attr *A : RD->attrs())
      N->addAttr(A->clone(Ctx));
    for (const FieldDecl *F : fields) {
      auto *NF = FieldDecl::Create(Ctx, N, F->getBeginLoc(), F->getLocation(),
                                   F->getIdentifier(), F->getType(),
                                   F->getTypeSourceInfo(), F->getBitWidth(),
                                   F->isMutable(), ICIS_NoInit);
      NF->setAccess(F->getAccess());
      for (const Attr *A : F->attrs())
        NF->addAttr(A->clone(Ctx));
      N->addDecl(NF);
    }
    N->completeDefinition();
    // Keep -Wpadded and friends quiet about a record nobody wrote.
    DiagnosticsEngine &D = Ctx.getDiagnostics();
    bool suppressed = D.getSuppressAllDiagnostics();
    D.setSuppressAllDiagnostics(true);
    uint64_t size = Ctx.getASTRecordLayout(N).getSize().getQuantity();
    D.setSuppressAllDiagnostics(suppressed);
    return size;
  }

  bool msABI() const { return ms; }
  unsigned pointerWidth() const { return pointerBits; }

  std::string recordName(const RecordDecl *RD) const {
    if (!RD->getIdentifier())
      if (const TypedefNameDecl *TD = RD->getTypedefNameForAnonDecl())
        return TD->getQualifiedNameAsString();
    // Clang prints a class declared in a function body without its scope,
    // and a class nested in one without the function's template arguments,
    // so `void f() { struct S; }` and a global S would both be "S". Name
    // local classes from their function instead: "ns::f<int>(char)::S::In".
    llvm::SmallVector<const RecordDecl *, 4> chain{RD};
    const FunctionDecl *F = nullptr;
    for (const DeclContext *P = RD->getDeclContext(); P; P = P->getParent()) {
      if ((F = dyn_cast<FunctionDecl>(P)))
        break;
      const auto *Outer = dyn_cast<RecordDecl>(P);
      if (!Outer)
        break;
      chain.push_back(Outer);
    }
    std::string s;
    llvm::raw_string_ostream os(s);
    if (!F) {
      RD->getNameForDiagnostic(os, policy, /*Qualified=*/true);
      return s;
    }
    os << functionScope(F);
    for (const RecordDecl *R : llvm::reverse(chain)) {
      os << "::";
      R->getNameForDiagnostic(os, policy, /*Qualified=*/false);
    }
    return s;
  }

  // "ns::C::f<int>(int, char)", the scope of classes local to f.
  std::string functionScope(const FunctionDecl *FD) const {
    std::string s;
    llvm::raw_string_ostream os(s);
    const auto *Owner = dyn_cast<CXXRecordDecl>(FD->getDeclContext());
    if (Owner && Owner->isLambda()) {
      // Two lambdas in one scope would both print as "(lambda)".
      PresumedLoc PL = C.getSourceManager().getPresumedLoc(Owner->getLocation());
      os << "(lambda at " << PL.getLine() << ':' << PL.getColumn() << ")::";
      FD->getNameForDiagnostic(os, policy, /*Qualified=*/false);
    } else if (Owner) {
      os << recordName(Owner) << "::";
      FD->getNameForDiagnostic(os, policy, /*Qualified=*/false);
    } else {
      FD->getNameForDiagnostic(os, policy, /*Qualified=*/true);
    }
    os << '(';
    for (unsigned i = 0; i < FD->getNumParams(); ++i)
      os << (i ? ", " : "") << typeName(FD->getParamDecl(i)->getType());
    os << ')';
    return s;
  }

private:
  ASTContext &C;
  const AnalyzeOptions &opts;
  PrintingPolicy policy;
  bool ms = false;
  unsigned pointerBits = 64;
  uint64_t pointerAlign = 64;

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
                    bool includeVBases) {
    const ASTRecordLayout &L = C.getASTRecordLayout(RD);
    auto *CXX = dyn_cast<CXXRecordDecl>(RD);
    N.kind = NodeKind::Record;
    N.offsetBits = offsetBits;
    N.tag = RD->getKindName().str();
    if (N.type.empty())
      N.type = recordName(RD);
    N.empty = CXX && CXX->isEmpty();

    // Reserve for every child pushed below, plus one padding node from
    // placeHole, so the vector does not move its Nodes as it grows.
    size_t children = std::distance(RD->field_begin(), RD->field_end()) + 1;
    if (CXX)
      children += 2 + CXX->getNumBases() +
                  (includeVBases ? 2 * CXX->getNumVBases() : 0);
    N.children.reserve(children);

    if (CXX) {
      const CXXRecordDecl *primary = L.getPrimaryBase();
      // Itanium: a dynamic class with no primary base owns a vtable pointer.
      if (ms ? L.hasOwnVFPtr() : CXX->isDynamicClass() && !primary)
        N.children.push_back(vptr(CXX, offsetBits));

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
        layoutRecord(child, B, offsetBits + C.toBits(L.getBaseClassOffset(B)),
                     false);
        child.alignBits =
            C.toBits(C.getASTRecordLayout(B).getNonVirtualAlignment());
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
        n.alignBits = pointerAlign;
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
        // Unnamed bitfields do not raise the record's alignment on Itanium.
        if (F->getIdentifier())
          n.alignBits = fieldAlign(F);
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
          layoutRecord(child, def, fieldBits, true);
          child.alignBits = fieldAlign(F);
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
      n.alignBits = fieldAlign(F);
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
          n.alignBits = 32;
          n.type = recordName(VB);
          N.children.push_back(std::move(n));
        }
        Node child;
        child.role = VB == L.getPrimaryBase() ? Role::PrimaryVirtualBase
                                              : Role::VirtualBase;
        layoutRecord(child, VB, vbBits, false);
        child.alignBits =
            C.toBits(C.getASTRecordLayout(VB).getNonVirtualAlignment());
        child.sizeBits =
            C.toBits(C.getASTRecordLayout(VB).getNonVirtualSize());
        N.children.push_back(std::move(child));
      }
    }
  }

  // An Itanium vtable pointer or a Microsoft vftable pointer.
  Node vptr(const CXXRecordDecl *owner, uint64_t offsetBits) {
    Node n = leaf(NodeKind::VPtr, offsetBits, pointerBits);
    n.type = recordName(owner);
    n.alignBits = pointerAlign;
    return n;
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
  report.bigEndian = C.getTargetInfo().isBigEndian();
  report.cacheLine = opts.cacheLine;

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
