#pragma once

#include "Model.h"

namespace llvm {
class raw_ostream;
}

namespace lense {

void printJson(llvm::raw_ostream &os, const Report &report);

} // namespace lense
