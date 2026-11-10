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
  // Fill Node::range for each member written in the main file.
  bool ranges = false;
  // Mark leaves that cross a boundary of this many bytes. 0 turns it off.
  unsigned cacheLine = 64;
  // Look for a field order that needs less padding.
  bool reorder = true;
  // Qualified names to report. Empty means every record in the main file.
  std::vector<std::string> records;
};

// With walkPreamble false, declarations from a precompiled preamble are not
// searched for records, unless their location is in the main file. Only
// pass false when no record in the main file was instantiated inside one.
Report analyze(clang::ASTContext &C, const AnalyzeOptions &opts,
               bool walkPreamble = true);

} // namespace lense
