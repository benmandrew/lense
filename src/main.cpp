// lense: show the memory layout of C and C++ records and their vtables.
#include "Json.h"
#include "Run.h"
#include "Text.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>

namespace cl = llvm::cl;

static cl::OptionCategory category("lense options");

static cl::opt<std::string> input(cl::Positional, cl::desc("<file | ->"),
                                  cl::Required, cl::cat(category));
static cl::opt<bool> json("json", cl::desc("Print JSON instead of text"),
                          cl::cat(category));
static cl::opt<bool> vtables("vtables",
                             cl::desc("Also print vtable layouts and the slot "
                                      "each vptr points at"),
                             cl::cat(category));
static cl::opt<bool>
    expandSystem("expand-system",
                 cl::desc("Expand members whose type is from a system header"),
                 cl::cat(category));
static cl::opt<bool> source("source",
                            cl::desc("Print each record's definition above "
                                     "its layout"),
                            cl::cat(category));
static cl::list<std::string>
    records("record", cl::desc("Only show this record (qualified name)"),
            cl::value_desc("name"), cl::CommaSeparated, cl::cat(category));
static cl::opt<std::string> target("target",
                                   cl::desc("Target triple, e.g. "
                                            "x86_64-pc-windows-msvc"),
                                   cl::value_desc("triple"), cl::cat(category));
static cl::opt<std::string>
    lang("lang", cl::desc("Language of standard input: c or c++"),
         cl::init("c++"), cl::cat(category));

int main(int argc, char **argv) {
  // Everything after "--" goes to clang untouched.
  std::vector<std::string> clangArgs;
  int ownArgc = argc;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--") == 0) {
      ownArgc = i;
      clangArgs.assign(argv + i + 1, argv + argc);
      break;
    }
  }
  // LLVM supplies --color (auto-detected when absent).
  cl::HideUnrelatedOptions({&category, &llvm::getColorCategory()});
  cl::SetVersionPrinter(
      [](llvm::raw_ostream &os) { os << "lense 0.1.0 (LLVM " LLVM_VERSION_STRING ")\n"; });
  cl::ParseCommandLineOptions(
      ownArgc, argv,
      "Show the memory layout of C and C++ records and their vtables.\n\n"
      "  lense [options] <file | -> [-- <clang arguments>]\n");

  if (lang != "c" && lang != "c++") {
    llvm::errs() << "lense: --lang must be c or c++\n";
    return 2;
  }

  auto buffer = llvm::MemoryBuffer::getFileOrSTDIN(input);
  if (!buffer) {
    llvm::errs() << "lense: cannot read " << input << ": "
                 << buffer.getError().message() << '\n';
    return 2;
  }

  std::string fileName;
  if (input == "-") {
    fileName = lang == "c" ? "stdin.c" : "stdin.cpp";
  } else {
    llvm::SmallString<256> abs(input.getValue());
    llvm::sys::fs::make_absolute(abs);
    fileName = abs.str().str();
  }

  lense::RunOptions opts;
  opts.analyze.vtables = vtables;
  opts.analyze.expandSystem = expandSystem;
  opts.analyze.source = source;
  opts.analyze.records.assign(records.begin(), records.end());
  if (!target.empty())
    opts.clangArgs.push_back("--target=" + target);
  opts.clangArgs.insert(opts.clangArgs.end(), clangArgs.begin(),
                        clangArgs.end());
  opts.colorDiagnostics = llvm::sys::Process::StandardErrHasColors();

  lense::Report report;
  bool ok = lense::run((*buffer)->getBuffer(), fileName, opts, report);

  llvm::raw_ostream &os = llvm::outs();
  if (json) {
    lense::printJson(os, report);
  } else {
    os.enable_colors(llvm::WithColor(os).colorsEnabled());
    lense::printText(os, report);
  }
  return ok ? 0 : 1;
}
