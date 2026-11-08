#pragma once

#include "Model.h"

namespace llvm {
class raw_ostream;
}

namespace lense {

// Colours follow the stream's settings; call os.enable_colors() first.
void printText(llvm::raw_ostream &os, const Report &report);
// Each record of `a` beside the same record of `b`, for --compare.
void printTextCompare(llvm::raw_ostream &os, const Report &a, const Report &b);

} // namespace lense
