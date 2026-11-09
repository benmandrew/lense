// Checks that runs through a PreambleCache report exactly what an uncached
// run reports: on the first run (which builds the preamble), on a repeat
// (which reuses it), and after an edit below the #include block.
//
//   lense_preamble_test <case>...
#include "Json.h"
#include "Run.h"

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

namespace {

std::string analyze(const std::string &code, llvm::StringRef file,
                    const lense::RunOptions &opts, lense::PreambleCache *cache) {
  lense::Report report;
  bool ok = lense::run(code, file, opts, report, cache);
  std::string s;
  llvm::raw_string_ostream os(s);
  os << (ok ? "ok " : "errors ");
  lense::printJson(os, report);
  return s;
}

} // namespace

int main(int argc, char **argv) {
  int failures = 0;
  for (const char *target : {"x86_64-linux-gnu", "x86_64-pc-windows-msvc"}) {
    lense::RunOptions opts;
    opts.clangArgs = {std::string("--target=") + target};
    lense::PreambleCache cache; // Shared by every case, as in the browser.
    for (int i = 1; i < argc; ++i) {
      auto buffer = llvm::MemoryBuffer::getFile(argv[i]);
      if (!buffer) {
        llvm::errs() << argv[i] << ": cannot read\n";
        return 2;
      }
      llvm::StringRef file = llvm::StringRef(argv[i]).ends_with(".c")
                                 ? "input.c" : "input.cpp";
      std::string code = (*buffer)->getBuffer().str();
      const std::string tail = "\nstruct PreambleTestTail { char c; size_t n; };\n";
      // clang's own stddef.h and stdarg.h keep the test host-independent.
      std::string headers = "#include <stddef.h>\n#include <stdarg.h>\n" + code;
      std::string edited = headers + tail;
      // A warning or an error in the preamble: neither may be cached.
      std::string warning = "#warning preamble\n" + code;
      std::string error = "#include \"lense_missing.h\"\n" + code;
      for (const std::string *v : {&code, &headers, &edited, &warning, &error}) {
        std::string want = analyze(*v, file, opts, nullptr);
        for (const char *pass : {"first", "repeat"}) {
          unsigned before = cache.reused;
          std::string got = analyze(*v, file, opts, &cache);
          if (got != want) {
            size_t at = 0;
            while (at < got.size() && at < want.size() && got[at] == want[at])
              ++at;
            at = at > 200 ? at - 200 : 0;
            llvm::errs() << argv[i] << " (" << target << ", " << pass
                         << " cached run) differs for:\n"
                         << v->substr(0, 120) << "\n--- uncached:\n"
                         << want.substr(at, 2000) << "\n--- cached:\n"
                         << got.substr(at, 2000) << "\n";
            ++failures;
          }
          // An edit keeps the preamble of the variant before it. The
          // preambles of `warning` and `error` have diagnostics, so they are
          // never used.
          bool repeat = llvm::StringRef(pass) == "repeat";
          bool shouldReuse = v == &edited || (v == &headers && repeat);
          if ((cache.reused != before) != shouldReuse) {
            llvm::errs() << argv[i] << " (" << target << ", " << pass
                         << " cached run): preamble "
                         << (shouldReuse ? "not reused" : "reused")
                         << " for:\n" << v->substr(0, 120) << "\n";
            ++failures;
          }
        }
      }
    }
  }
  return failures != 0;
}
