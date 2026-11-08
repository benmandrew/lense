#pragma once

#include "Model.h"

namespace clang {
class ASTContext;
}

namespace lense {

struct AnalyzeOptions {
  bool vtables = true;
  // Expand fields whose class type comes from a system header.
  bool expandSystem = false;
  // Copy each record's definition into Record::source.
  bool source = false;
  // Qualified names to report. Empty means every record in the main file.
  std::vector<std::string> records;
};

Report analyze(clang::ASTContext &C, const AnalyzeOptions &opts);

} // namespace lense
