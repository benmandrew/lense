#pragma once

#include "Model.h"

namespace llvm {
class raw_ostream;
}

namespace lense {

void printJson(llvm::raw_ostream &os, const Report &report);
// {"reports": [a, b]}, for --compare.
void printJsonCompare(llvm::raw_ostream &os, const Report &a, const Report &b);

} // namespace lense
