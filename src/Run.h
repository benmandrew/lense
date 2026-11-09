#pragma once

#include "Analyze.h"

#include "llvm/ADT/StringRef.h"

#include <memory>
#include <string>
#include <vector>

namespace lense {

struct RunOptions {
  AnalyzeOptions analyze;
  std::vector<std::string> clangArgs; // Passed to clang after the defaults.
  bool colorDiagnostics = false;
};

// Keeps the compiled #include block at the top of a file (its preamble)
// between runs, so that a run whose preamble and options are unchanged
// skips the headers. Holds one preamble per set of clang options, for the
// two most recent sets.
class PreambleCache {
public:
  PreambleCache();
  ~PreambleCache();
  unsigned reused = 0; // Runs that used a preamble built by an earlier run.
  struct Impl;
  std::unique_ptr<Impl> impl;
};

// Parses `code` as if it were the file `fileName` and fills `out`. Returns
// false if clang reported errors; `out` still holds every valid record.
bool run(llvm::StringRef code, llvm::StringRef fileName, const RunOptions &opts,
         Report &out, PreambleCache *cache = nullptr);

} // namespace lense
