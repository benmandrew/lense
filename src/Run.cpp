#include "Run.h"
#include "lense_config.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

namespace lense {
namespace {

class Consumer : public clang::ASTConsumer {
public:
  Consumer(const AnalyzeOptions &opts, Report &out) : opts(opts), out(out) {}
  void HandleTranslationUnit(clang::ASTContext &C) override {
    out = analyze(C, opts);
  }

private:
  const AnalyzeOptions &opts;
  Report &out;
};

class Action : public clang::ASTFrontendAction {
public:
  Action(const AnalyzeOptions &opts, Report &out) : opts(opts), out(out) {}
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &, llvm::StringRef) override {
    return std::make_unique<Consumer>(opts, out);
  }

private:
  const AnalyzeOptions &opts;
  Report &out;
};

bool hasFlag(const std::vector<std::string> &args,
             std::initializer_list<llvm::StringRef> prefixes) {
  for (const std::string &a : args)
    for (llvm::StringRef p : prefixes)
      if (llvm::StringRef(a).starts_with(p))
        return true;
  return false;
}

// Returns the value of --target=, -target or --target, or "" if absent.
std::string findTarget(const std::vector<std::string> &args) {
  for (size_t i = 0; i < args.size(); ++i) {
    llvm::StringRef a = args[i];
    if (a.consume_front("--target="))
      return a.str();
    if ((a == "-target" || a == "--target") && i + 1 < args.size())
      return args[i + 1];
  }
  return "";
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

bool run(llvm::StringRef code, llvm::StringRef fileName, const RunOptions &opts,
         Report &out) {
  return clang::tooling::runToolOnCodeWithArgs(
      std::make_unique<Action>(opts.analyze, out), code, buildArgs(opts),
      fileName, "lense");
}

} // namespace lense
