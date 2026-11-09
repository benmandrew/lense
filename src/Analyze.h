#pragma once

#include "Model.h"

namespace clang {
class ASTContext;
}

namespace lense {

struct AnalyzeOptions {
  // Expand fields whose class type comes from a system header.
  bool expandSystem = false;
  // Copy each record's definition into Record::source.
  bool source = false;
  // Mark leaves that cross a boundary of this many bytes. 0 turns it off.
  unsigned cacheLine = 64;
  // Look for a field order that needs less padding.
  bool reorder = true;
  // Qualified names to report. Empty means every record in the main file.
  std::vector<std::string> records;
};

Report analyze(clang::ASTContext &C, const AnalyzeOptions &opts);

} // namespace lense
