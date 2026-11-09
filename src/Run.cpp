#include "Run.h"
#include "lense_config.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/PrecompiledPreamble.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/VirtualFileSystem.h"

#include <optional>

namespace lense {

struct PreambleCache::Impl {
  struct Entry {
    std::vector<std::string> cc1; // The options the preamble was built with.
    std::string text;             // The preamble's source text.
    // Empty if building it produced diagnostics: a reused preamble would not
    // repeat them, so such files are always parsed in full.
    std::optional<clang::PrecompiledPreamble> pch;
  };
  std::vector<Entry> entries; // Most recently used last.

  // Returns the preamble to use for `main` under `CI`, building it if needed,
  // or null to parse the whole file.
  const clang::PrecompiledPreamble *
  get(unsigned &reused, clang::CompilerInvocation &CI,
      const llvm::MemoryBuffer &main,
      llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS,
      std::shared_ptr<clang::PCHContainerOperations> PCH) {
    clang::PreambleBounds bounds =
        clang::ComputePreambleBounds(CI.getLangOpts(), main, 0);
    if (bounds.Size == 0)
      return nullptr;
    llvm::StringRef text = main.getBuffer().take_front(bounds.Size);
    std::vector<std::string> cc1 = CI.getCC1CommandLine();
    auto it = llvm::find_if(entries, [&](const Entry &e) { return e.cc1 == cc1; });
    if (it != entries.end()) {
      Entry e = std::move(*it);
      entries.erase(it);
      bool reuse = e.pch ? e.pch->CanReuse(CI, main, bounds, *VFS)
                         : e.text == text;
      if (reuse) {
        entries.push_back(std::move(e));
        if (!entries.back().pch)
          return nullptr;
        ++reused;
        return &*entries.back().pch;
      }
    }
    // Notes whether there were warnings or errors. The diagnostic counts
    // will not do: the preamble's PCH writer clears them.
    struct Watcher : clang::DiagnosticConsumer {
      bool any = false;
      void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                            const clang::Diagnostic &) override {
        any |= level >= clang::DiagnosticsEngine::Warning;
      }
    } watcher;
    clang::PreambleCallbacks callbacks;
    auto built = clang::PrecompiledPreamble::Build(
        CI, &main, bounds,
        clang::CompilerInstance::createDiagnostics(*VFS, CI.getDiagnosticOpts(),
                                                   &watcher, false),
        VFS, std::move(PCH), /*StoreInMemory=*/true, "", callbacks);
    Entry e{std::move(cc1), text.str(), std::nullopt};
    if (built && !watcher.any)
      e.pch = std::move(*built);
    if (entries.size() >= 2)
      entries.erase(entries.begin());
    entries.push_back(std::move(e));
    return entries.back().pch ? &*entries.back().pch : nullptr;
  }
};

PreambleCache::PreambleCache() : impl(std::make_unique<Impl>()) {}
PreambleCache::~PreambleCache() = default;

namespace {

class Consumer : public clang::ASTConsumer {
public:
  Consumer(const AnalyzeOptions &opts, Report &out) : opts(opts), out(out) {}
  void HandleTranslationUnit(clang::ASTContext &C) override {
    out = analyze(C, opts);
  }
  // Only records in the main file are reported, and clang already keeps
  // bodies it needs (constexpr, deduced return types).
  bool shouldSkipFunctionBody(clang::Decl *D) override {
    const clang::SourceManager &SM = D->getASTContext().getSourceManager();
    return !SM.isInMainFile(SM.getExpansionLoc(D->getLocation()));
  }

private:
  const AnalyzeOptions &opts;
  Report &out;
};

class Action : public clang::ASTFrontendAction {
public:
  Action(const AnalyzeOptions &opts, Report &out) : opts(opts), out(out) {}
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &CI, llvm::StringRef) override {
    CI.getFrontendOpts().SkipFunctionBodies = true;
    return std::make_unique<Consumer>(opts, out);
  }

private:
  const AnalyzeOptions &opts;
  Report &out;
};

// What clang::tooling::FrontendActionFactory::runInvocation does, plus the
// preamble.
class Tool : public clang::tooling::ToolAction {
public:
  Tool(const AnalyzeOptions &opts, Report &out, llvm::StringRef code,
       PreambleCache *cache)
      : opts(opts), out(out), code(code), cache(cache) {}

  bool runInvocation(std::shared_ptr<clang::CompilerInvocation> CI,
                     clang::FileManager *Files,
                     std::shared_ptr<clang::PCHContainerOperations> PCH,
                     clang::DiagnosticConsumer *DiagConsumer) override {
    CI->getFrontendOpts().SkipFunctionBodies = true;
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS =
        Files->getVirtualFileSystemPtr();
    // Outlives the compiler instance, which reads it as the main file.
    std::unique_ptr<llvm::MemoryBuffer> main;
    const clang::PrecompiledPreamble *preamble = nullptr;
    if (cache) {
      main = llvm::MemoryBuffer::getMemBufferCopy(
          code, CI->getFrontendOpts().Inputs[0].getFile());
      preamble = cache->impl->get(cache->reused, *CI, *main, VFS, PCH);
    }
    if (preamble) {
      preamble->AddImplicitPreamble(*CI, VFS, main.get());
      CI->getPreprocessorOpts().RetainRemappedFileBuffers = true;
    }
    clang::CompilerInstance Compiler(std::move(CI), std::move(PCH));
    Compiler.setVirtualFileSystem(VFS);
    if (preamble)
      Compiler.createFileManager(); // VFS now also holds the preamble.
    else
      Compiler.setFileManager(Files);
    Compiler.createDiagnostics(DiagConsumer, /*ShouldOwnClient=*/false);
    Compiler.createSourceManager();
    Action action(opts, out);
    return Compiler.ExecuteAction(action);
  }

private:
  const AnalyzeOptions &opts;
  Report &out;
  llvm::StringRef code;
  PreambleCache *cache;
};

bool hasFlag(const std::vector<std::string> &args,
             std::initializer_list<llvm::StringRef> prefixes) {
  for (const std::string &a : args)
    for (llvm::StringRef p : prefixes)
      if (llvm::StringRef(a).starts_with(p))
        return true;
  return false;
}

// Returns the value of the last --target=, -target or --target, or "" if
// absent. Clang also takes the last one.
std::string findTarget(const std::vector<std::string> &args) {
  std::string target;
  for (size_t i = 0; i < args.size(); ++i) {
    llvm::StringRef a = args[i];
    if (a.consume_front("--target="))
      target = a.str();
    else if ((a == "-target" || a == "--target") && i + 1 < args.size())
      target = args[i + 1];
  }
  return target;
}

std::vector<std::string> buildArgs(const RunOptions &opts) {
  std::vector<std::string> args;
  const std::vector<std::string> &user = opts.clangArgs;
  if (!hasFlag(user, {"-resource-dir"}))
    args.push_back("-resource-dir=" LENSE_RESOURCE_DIR);

  // Host header search only makes sense when compiling for the host (or, on
  // macOS, another Apple target that shares the SDK).
  llvm::StringRef sysroot = LENSE_DEFAULT_SYSROOT;
  std::string target = findTarget(user);
  bool hostLike = LENSE_HOST_ARGS_ANY_TARGET || target.empty() ||
                  (!sysroot.empty() && llvm::StringRef(target).contains("apple"));
  if (hostLike) {
    if (!sysroot.empty() && !hasFlag(user, {"-isysroot", "--sysroot"})) {
      args.push_back("-isysroot");
      args.push_back(sysroot.str());
    }
    std::initializer_list<const char *> hostArgs = LENSE_HOST_ARGS;
    args.insert(args.end(), hostArgs.begin(), hostArgs.end());
  }
  if (opts.colorDiagnostics)
    args.push_back("-fcolor-diagnostics");
  args.insert(args.end(), user.begin(), user.end());
  return args;
}

} // namespace

// The setup of clang::tooling::runToolOnCodeWithArgs, with Tool as the action.
bool run(llvm::StringRef code, llvm::StringRef fileName, const RunOptions &opts,
         Report &out, PreambleCache *cache) {
  auto overlay = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(
      llvm::vfs::getRealFileSystem());
  auto memory = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
  overlay->pushOverlay(memory);
  memory->addFile(fileName, 0, llvm::MemoryBuffer::getMemBufferCopy(code));
  auto files = llvm::makeIntrusiveRefCnt<clang::FileManager>(
      clang::FileSystemOptions(), overlay);

  std::vector<std::string> args = {"lense", "-fsyntax-only"};
  llvm::append_range(args, clang::tooling::getClangStripDependencyFileAdjuster()(
                               buildArgs(opts), fileName));
  args.push_back(fileName.str());
  Tool tool(opts.analyze, out, code, cache);
  return clang::tooling::ToolInvocation(
             std::move(args), &tool, files.get(),
             std::make_shared<clang::PCHContainerOperations>())
      .run();
}

} // namespace lense
