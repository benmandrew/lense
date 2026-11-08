#pragma once

#include "Analyze.h"

#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace lense {

struct RunOptions {
  AnalyzeOptions analyze;
  std::vector<std::string> clangArgs; // Passed to clang after the defaults.
  bool colorDiagnostics = false;
};

// Parses `code` as if it were the file `fileName` and fills `out`. Returns
// false if clang reported errors; `out` still holds every valid record.
bool run(llvm::StringRef code, llvm::StringRef fileName, const RunOptions &opts,
         Report &out);

} // namespace lense
