// Walks the AST and turns clang's record layouts into the model.
// The traversal order follows clang's own -fdump-record-layouts.
#include "Analyze.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <numeric>

using namespace clang;

namespace lense {
namespace {

// A run of fields that moves as one when reordering: a single field, or
// adjacent bitfields, which would pack differently if split.
struct Unit {
  std::vector<const FieldDecl *> fields;
  uint64_t sizeBits = 0;
  uint64_t alignBits = 8;
};

struct Analyzer {
  ASTContext &C;
  const AnalyzeOptions &opts;
  PrintingPolicy policy;
  bool ms;
  unsigned pointerBits;
  uint64_t pointerAlign;

  Analyzer(ASTContext &C, const AnalyzeOptions &opts)
      : C(C), opts(opts), policy(C.getPrintingPolicy()),
        ms(C.getTargetInfo().getCXXABI().isMicrosoft()),
        pointerBits(C.getTargetInfo().getPointerWidth(LangAS::Default)),
        pointerAlign(C.getTargetInfo().getPointerAlign(LangAS::Default)) {
    policy.AnonymousTagNameStyle =
        llvm::to_underlying(PrintingPolicy::AnonymousTagMode::Plain);
    policy.SuppressTemplateArgsInCXXConstructors = true;
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
    size_t lineStart = buf.rfind('\n', begin) + 1; // npos + 1 is 0.
    llvm::StringRef before = buf.slice(lineStart, begin);
    std::string text = before.trim(" \t").empty()
                           ? before.str()
                           : std::string(begin - lineStart, ' ');
    // Keep the semicolon that closes the definition.
    size_t stop = buf.find_first_not_of(" \t", endOff);
    if (stop < buf.size() && buf[stop] == ';')
      endOff = stop + 1;
    text += buf.slice(begin, endOff);

    llvm::SmallVector<llvm::StringRef, 16> lines;
    llvm::StringRef(text).split(lines, '\n');
    size_t indent = llvm::StringRef::npos;
    for (llvm::StringRef &l : lines) {
      l = l.rtrim(" \t\r");
      if (!l.empty())
        indent = std::min(indent, l.find_first_not_of(" \t"));
    }
    for (llvm::StringRef l : lines) {
      R.source += l.drop_front(std::min(indent, l.size()));
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
        if (A->isAlignmentDependent() || A->getAlignment(C) != attr)
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
    auto c = llvm::find_if(R.layout.children,
                           [&](const Node &c) { return c.alignBits == align; });
    if (c == R.layout.children.end())
      return;
    R.alignSource = AlignSource::Member;
    switch (c->kind) {
    case NodeKind::VPtr: R.alignText = ms ? "vfptr" : "vptr"; break;
    case NodeKind::VBPtr: R.alignText = "vbptr"; break;
    case NodeKind::VtorDisp: R.alignText = "vtordisp"; break;
    case NodeKind::Record:
      if (c->role != Role::Field) {
        bool isVirtual = c->role == Role::VirtualBase ||
                         c->role == Role::PrimaryVirtualBase;
        R.alignText = (isVirtual ? "virtual base " : "base ") + c->type;
        break;
      }
      [[fallthrough]];
    default:
      R.alignText = c->name.empty() ? c->type : c->name;
    }
  }

  // Marks unaligned members and those that cross a cache line, and numbers
  // the memory locations of bitfields. A memory location is a maximal run of
  // adjacent bitfields of nonzero width ([intro.memory]).
  void annotate(Node &N, uint64_t rootAlign, unsigned &location) const {
    uint64_t line = uint64_t(opts.cacheLine) * 8;
    bool isUnion = N.tag == "union";
    bool run = false;
    for (Node &c : N.children) {
      bool bit = c.kind == NodeKind::BitField && c.sizeBits;
      if (bit) {
        if (!run)
          ++location;
        c.location = location;
      }
      run = bit && !isUnion;
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
    const auto *CXX = dyn_cast<CXXRecordDecl>(RD);
    return {.standardLayout = !CXX || CXX->isStandardLayout(),
            .triviallyCopyable = T.isTriviallyCopyableType(C),
            .uniqueObjectRepresentations = C.hasUniqueObjectRepresentations(T)};
  }

  // The alignment a field asks for: its type's, raised by alignas on it.
  uint64_t fieldAlign(const FieldDecl *F) const {
    QualType T = F->getType();
    if (T->isIncompleteArrayType())
      T = C.getBaseElementType(T);
    uint64_t a = T->isIncompleteType() ? 8 : C.getTypeAlign(T);
    return std::max<uint64_t>(a, F->getMaxAlignment());
  }

  // The bits a non-bitfield field owns. A [[no_unique_address]] member may
  // share its tail padding with later members, so it only owns its data
  // size (zero if empty).
  uint64_t fieldSize(const FieldDecl *F) const {
    if (F->isZeroSize(C))
      return 0;
    if (const RecordDecl *FR = F->getType()->getAsRecordDecl();
        FR && F->isPotentiallyOverlapping())
      return C.toBits(C.getASTRecordLayout(FR).getDataSize());
    return C.getTypeSize(F->getType());
  }

  // -- Field reordering ----------------------------------------------------

  // Estimates the size of RD with its fields in `order`, starting at `start`
  // bits. Clang checks the winner, so this only has to rank orders.
  static uint64_t estimate(const std::vector<Unit> &units,
                           const std::vector<unsigned> &order, uint64_t start,
                           uint64_t align) {
    uint64_t pos = start;
    for (unsigned u : order)
      pos = llvm::alignTo(pos, units[u].alignBits) + units[u].sizeBits;
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

    bool packed = RD->hasAttr<PackedAttr>();
    uint64_t maxField = packed ? 8 : 0;
    if (const auto *M = RD->getAttr<MaxFieldAlignmentAttr>())
      maxField = M->getAlignment();
    std::vector<Unit> units;
    const FieldDecl *flexible = nullptr;
    bool inRun = false;
    for (const FieldDecl *F : RD->fields()) {
      if (F->getType()->isIncompleteArrayType()) {
        flexible = F;
        continue;
      }
      uint64_t a = fieldAlign(F);
      if (packed || F->hasAttr<PackedAttr>())
        a = std::max<uint64_t>(8, F->getMaxAlignment());
      if (maxField)
        a = std::min(a, maxField);
      bool bit = F->isBitField();
      if (!bit || !inRun)
        units.emplace_back();
      inRun = bit;
      Unit &u = units.back();
      u.fields.push_back(F);
      u.alignBits = std::max(u.alignBits, a);
      // A run of bitfields spans from its first bit to its last.
      u.sizeBits = bit ? L.getFieldOffset(F->getFieldIndex()) +
                             F->getBitWidthValue() -
                             L.getFieldOffset(u.fields[0]->getFieldIndex())
                       : fieldSize(F);
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
      for (unsigned u : order)
        llvm::append_range(fields, units[u].fields);
      if (flexible)
        fields.push_back(flexible);
      uint64_t size = sizeWithFields(RD, fields);
      if (size >= R.size || (best && size >= best->size))
        continue;
      best.emplace();
      best->size = size;
      for (const FieldDecl *F : fields) {
        best->order.push_back(F->getFieldIndex());
        best->fields.push_back(fieldLabel(F));
      }
    }
    return best;
  }

  // The size clang gives a copy of RD whose fields are `fields`, in order.
  // The copy keeps RD's bases, attributes and own vptr, but no other
  // members; RD has no virtual bases, so nothing else affects the layout.
  uint64_t sizeWithFields(const RecordDecl *RD,
                          llvm::ArrayRef<const FieldDecl *> fields) {
    DeclContext *DC = const_cast<DeclContext *>(RD->getDeclContext());
    SourceLocation loc = RD->getLocation();
    IdentifierInfo *id = RD->getIdentifier();
    const auto *CXX = dyn_cast<CXXRecordDecl>(RD);
    RecordDecl *N =
        CXX ? CXXRecordDecl::Create(C, RD->getTagKind(), DC, loc, loc, id)
            : RecordDecl::Create(C, RD->getTagKind(), DC, loc, loc, id);
    N->startDefinition();
    if (CXX) {
      auto *NC = cast<CXXRecordDecl>(N);
      std::vector<const CXXBaseSpecifier *> bases;
      for (const CXXBaseSpecifier &B : CXX->bases())
        bases.push_back(&B);
      NC->setBases(bases.data(), bases.size());
      bool ownVPtr = ms ? C.getASTRecordLayout(RD).hasOwnVFPtr()
                        : CXX->isPolymorphic() && !NC->isPolymorphic();
      if (ownVPtr) {
        QualType FT = C.getFunctionType(C.VoidTy, {}, {});
        auto *MD = CXXMethodDecl::Create(
            C, NC, loc,
            DeclarationNameInfo(&C.Idents.get("__lense_virtual"), loc), FT,
            C.getTrivialTypeSourceInfo(FT), SC_None, false, false,
            ConstexprSpecKind::Unspecified, loc);
        MD->setVirtualAsWritten(true);
        MD->setAccess(AS_public);
        NC->addDecl(MD);
      }
    }
    N->setImplicit(true);
    for (const Attr *A : RD->attrs())
      N->addAttr(A->clone(C));
    for (const FieldDecl *F : fields) {
      auto *NF = FieldDecl::Create(C, N, F->getBeginLoc(), F->getLocation(),
                                   F->getIdentifier(), F->getType(),
                                   F->getTypeSourceInfo(), F->getBitWidth(),
                                   F->isMutable(), ICIS_NoInit);
      NF->setAccess(F->getAccess());
      for (const Attr *A : F->attrs())
        NF->addAttr(A->clone(C));
      N->addDecl(NF);
    }
    N->completeDefinition();
    // Keep -Wpadded and friends quiet about a record nobody wrote.
    DiagnosticsEngine &D = C.getDiagnostics();
    bool suppressed = D.getSuppressAllDiagnostics();
    D.setSuppressAllDiagnostics(true);
    uint64_t size = C.getASTRecordLayout(N).getSize().getQuantity();
    D.setSuppressAllDiagnostics(suppressed);
    return size;
  }

  // -- Names ---------------------------------------------------------------

  std::string typeName(QualType T) const { return T.getAsString(policy); }

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
    } else if (Owner) {
      os << recordName(Owner) << "::";
    }
    FD->getNameForDiagnostic(os, policy, /*Qualified=*/!Owner);
    os << '(';
    for (unsigned i = 0; i < FD->getNumParams(); ++i)
      os << (i ? ", " : "") << typeName(FD->getParamDecl(i)->getType());
    os << ')';
    return s;
  }

  // -- Source ranges -------------------------------------------------------

  // [begin, end) as written in the main file, with trailing blanks dropped.
  // With tokenEnd, end is the start of the last token instead.
  Range range(SourceLocation begin, SourceLocation end, bool tokenEnd) const {
    if (!opts.ranges || begin.isInvalid() || end.isInvalid())
      return {};
    const SourceManager &SM = C.getSourceManager();
    // Something from a macro is the whole macro call.
    begin = SM.getExpansionRange(begin).getBegin();
    if (tokenEnd)
      end = Lexer::getLocForEndOfToken(SM.getExpansionRange(end).getEnd(), 0,
                                       SM, C.getLangOpts());
    else
      end = SM.getExpansionLoc(end);
    auto [fid, b] = SM.getDecomposedLoc(begin);
    auto [endFid, e] = SM.getDecomposedLoc(end);
    if (fid != SM.getMainFileID() || endFid != fid || e <= b)
      return {};
    llvm::StringRef buf = SM.getBufferData(fid);
    while (e > b && llvm::isSpace(buf[e - 1]))
      --e;
    return {SM.getLineNumber(fid, b), SM.getColumnNumber(fid, b),
            SM.getLineNumber(fid, e), SM.getColumnNumber(fid, e)};
  }

  Range range(clang::SourceRange r) const {
    return range(r.getBegin(), r.getEnd(), true);
  }

  // A field from its type to the end of its declarator. In `int a, b;` the
  // type belongs to a, so b starts at its name. An anonymous struct or
  // union is its whole definition.
  Range fieldRange(const FieldDecl *F, const FieldDecl *prev) const {
    if (F->isAnonymousStructOrUnion())
      return range(F->getType()->getAsRecordDecl()->getSourceRange());
    SourceLocation begin = F->getBeginLoc();
    if (prev && prev->getBeginLoc() == begin)
      begin = F->getLocation();
    return range(begin, F->getEndLoc(), true);
  }

  // A virtual function up to its body, if it has one there. A member of a
  // class template instantiation may not have its body yet, so look at the
  // template's.
  Range methodRange(const CXXMethodDecl *M) const {
    const FunctionDecl *F = M->getTemplateInstantiationPattern();
    if (!F)
      F = M;
    if (const Stmt *body = F->getBody();
        body && F->doesThisDeclarationHaveABody())
      return range(F->getBeginLoc(), body->getBeginLoc(), false);
    return range(F->getSourceRange());
  }

  // What gives CXX its own vptr: the first virtual function it declares that
  // overrides nothing, else its first virtual function, else its first
  // virtual base.
  Range vptrRange(const CXXRecordDecl *CXX) const {
    const CXXMethodDecl *first = nullptr;
    for (const CXXMethodDecl *M : CXX->methods()) {
      if (M->isImplicit() || !M->isVirtual())
        continue;
      if (M->size_overridden_methods() == 0)
        return methodRange(M);
      if (!first)
        first = M;
    }
    return first ? methodRange(first) : vbptrRange(CXX);
  }

  Range vbptrRange(const CXXRecordDecl *CXX) const {
    for (const CXXBaseSpecifier &B : CXX->bases())
      if (B.isVirtual())
        return range(B.getSourceRange());
    return {};
  }

  // -- Record layout -------------------------------------------------------

  static Node leaf(NodeKind kind, uint64_t offsetBits, uint64_t sizeBits,
                   uint64_t alignBits = 0, std::string type = "",
                   std::string name = "") {
    Node n;
    n.kind = kind;
    n.offsetBits = offsetBits;
    n.sizeBits = sizeBits;
    n.alignBits = alignBits;
    n.type = std::move(type);
    n.name = std::move(name);
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
      if (ms ? L.hasOwnVFPtr() : CXX->isDynamicClass() && !primary) {
        N.children.push_back(leaf(NodeKind::VPtr, offsetBits, pointerBits,
                                  pointerAlign, recordName(CXX)));
        N.children.back().range = vptrRange(CXX);
      }

      std::vector<const CXXBaseSpecifier *> bases;
      for (const CXXBaseSpecifier &B : CXX->bases())
        if (!B.isVirtual())
          bases.push_back(&B);
      auto offset = [&](const CXXBaseSpecifier *B) {
        return L.getBaseClassOffset(B->getType()->getAsCXXRecordDecl());
      };
      std::stable_sort(bases.begin(), bases.end(),
                       [&](const CXXBaseSpecifier *a,
                           const CXXBaseSpecifier *b) {
                         return offset(a) < offset(b);
                       });
      for (const CXXBaseSpecifier *B : bases) {
        const CXXRecordDecl *BD = B->getType()->getAsCXXRecordDecl();
        N.children.push_back(
            base(BD, BD == primary ? Role::PrimaryBase : Role::Base,
                 offsetBits + C.toBits(offset(B))));
        N.children.back().range = range(B->getSourceRange());
      }

      if (L.hasOwnVBPtr()) {
        N.children.push_back(leaf(NodeKind::VBPtr,
                                  offsetBits + C.toBits(L.getVBPtrOffset()),
                                  pointerBits, pointerAlign, recordName(CXX)));
        N.children.back().range = vbptrRange(CXX);
      }
    }

    unsigned fieldNo = 0;
    const FieldDecl *prev = nullptr;
    for (const FieldDecl *F : RD->fields()) {
      Range written = fieldRange(F, prev);
      prev = F;
      uint64_t fieldBits = offsetBits + L.getFieldOffset(fieldNo++);
      QualType T = F->getType();
      // A field of class type is laid out in place, unless the class comes
      // from a system header. Bitfields never have class type.
      const RecordDecl *def = T->getAsRecordDecl();
      if (def && def->getDefinition() &&
          (opts.expandSystem ||
           !C.getSourceManager().isInSystemHeader(def->getLocation()))) {
        Node child;
        child.role = Role::Field;
        child.name = F->getName().str();
        if (def->getIdentifier() || def->getTypedefNameForAnonDecl())
          child.type = typeName(T);
        else
          child.type = (child.name.empty() ? "(anonymous " : "(unnamed ") +
                       def->getKindName().str() + ")";
        layoutRecord(child, def, fieldBits, true);
        child.alignBits = fieldAlign(F);
        child.sizeBits = fieldSize(F);
        child.range = written;
        N.children.push_back(std::move(child));
        continue;
      }
      bool bit = F->isBitField();
      uint64_t size = bit                    ? F->getBitWidthValue()
                      : T->isIncompleteType() ? 0
                                              : C.getTypeSize(T);
      // Unnamed bitfields do not raise the record's alignment on Itanium.
      uint64_t align = bit && !F->getIdentifier() ? 0 : fieldAlign(F);
      N.children.push_back(leaf(bit ? NodeKind::BitField : NodeKind::Field,
                                fieldBits, size, align, typeName(T),
                                F->getName().str()));
      N.children.back().range = written;
    }

    if (CXX && includeVBases) {
      const auto &vbaseInfo = L.getVBaseOffsetsMap();
      for (const CXXBaseSpecifier &B : CXX->vbases()) {
        const CXXRecordDecl *VB = B.getType()->getAsCXXRecordDecl();
        uint64_t vbBits = offsetBits + C.toBits(L.getVBaseClassOffset(VB));
        auto it = vbaseInfo.find(VB);
        Range written = range(B.getSourceRange());
        if (it != vbaseInfo.end() && it->second.hasVtorDisp()) {
          N.children.push_back(
              leaf(NodeKind::VtorDisp, vbBits - 32, 32, 32, recordName(VB)));
          N.children.back().range = written;
        }
        N.children.push_back(base(VB,
                                  VB == L.getPrimaryBase()
                                      ? Role::PrimaryVirtualBase
                                      : Role::VirtualBase,
                                  vbBits));
        N.children.back().range = written;
      }
    }
  }

  // A base subobject, which owns only its non-virtual part. An empty
  // non-virtual base shares its address with other subobjects.
  Node base(const CXXRecordDecl *B, Role role, uint64_t offsetBits) {
    Node n;
    n.role = role;
    layoutRecord(n, B, offsetBits, false);
    const ASTRecordLayout &BL = C.getASTRecordLayout(B);
    bool isVirtual =
        role == Role::VirtualBase || role == Role::PrimaryVirtualBase;
    n.alignBits = C.toBits(BL.getNonVirtualAlignment());
    n.sizeBits = n.empty && !isVirtual ? 0 : C.toBits(BL.getNonVirtualSize());
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
    auto pos = std::upper_bound(
        N.children.begin(), N.children.end(), h.offsetBits,
        [](uint64_t off, const Node &c) { return off < c.offsetBits; });
    N.children.insert(pos, leaf(NodeKind::Padding, h.offsetBits, h.sizeBits));
  }
};

class Collector : public RecursiveASTVisitor<Collector> {
public:
  Collector(ASTContext &C, bool walkPreamble)
      : C(C), walkPreamble(walkPreamble) {}

  bool shouldVisitTemplateInstantiations() const { return true; }

  // A precompiled preamble holds headers, and walking it would deserialise
  // all of it. An explicit instantiation in the main file moves a header's
  // specialisation there, so those are still walked.
  bool TraverseDecl(Decl *D) {
    if (!walkPreamble && D && D->isFromASTFile() &&
        !C.getSourceManager().isInMainFile(D->getLocation()))
      return true;
    return RecursiveASTVisitor<Collector>::TraverseDecl(D);
  }

  bool VisitRecordDecl(RecordDecl *RD) {
    auto *CXX = dyn_cast<CXXRecordDecl>(RD);
    if (RD->isImplicit() || RD->isInvalidDecl() ||
        !RD->isThisDeclarationADefinition() || !RD->isCompleteDefinition() ||
        RD->isDependentType() || (CXX && CXX->isLambda()))
      return true;
    // Anonymous structs and unions appear inside their parent's layout.
    if (!RD->getIdentifier() && !RD->getTypedefNameForAnonDecl())
      return true;
    if (C.getSourceManager().isInMainFile(RD->getLocation()) &&
        seen.insert(RD).second)
      found.push_back(RD);
    return true;
  }

  std::vector<const RecordDecl *> found;

private:
  ASTContext &C;
  bool walkPreamble;
  llvm::DenseSet<const RecordDecl *> seen;
};

} // namespace

Report analyze(ASTContext &C, const AnalyzeOptions &opts, bool walkPreamble) {
  Analyzer A(C, opts);
  Report report;
  report.target = C.getTargetInfo().getTriple().str();
  report.abi = A.ms ? "microsoft" : "itanium";
  report.pointerBits = A.pointerBits;
  report.bigEndian = C.getTargetInfo().isBigEndian();
  report.cacheLine = opts.cacheLine;

  Collector collector(C, walkPreamble);
  collector.TraverseDecl(C.getTranslationUnitDecl());
  for (const RecordDecl *RD : collector.found)
    if (opts.records.empty() ||
        llvm::is_contained(opts.records, A.recordName(RD)))
      report.records.push_back(A.analyzeRecord(RD));
  return report;
}

} // namespace lense
