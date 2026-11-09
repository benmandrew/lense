// Checks that runs through a PreambleCache report exactly what an uncached
// run reports: on the first run (which builds the preamble), on a repeat
// (which reuses it), and after an edit below the #include block.
//
//   lense_preamble_test <case>...
#include "Json.h"
#include "Run.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace {

// Records in a header that the main file builds on. None of them is
// reported, but specialisations and instantiations of them may be.
const char *header = R"(#pragma once
struct pt_c { char a; int b; };
#ifdef __cplusplus
namespace pt {
template <class T> struct Box { T value; char tag; };
template <class T> struct Box<T *> { T *ptr; int extra; };
template <class T> struct Holder { T item; struct Inner { T a; char b; } in; };
struct Base { virtual ~Base(); int b; };
typedef char boxIntSize[sizeof(Box<int>)]; // Instantiated in the header...
}
#endif
)";

// Uses that lense finds without walking the preamble.
const char *uses = R"(#ifdef __cplusplus
template struct pt::Box<int>; // ...and explicitly in the main file.
template struct pt::Box<double>;
namespace pt {
struct Mine : Base { char m; };
template <> struct Box<char> { char only; };
}
pt::Box<int *> ptrBox;
pt::Holder<short> holder;
#endif
struct pt_main { struct pt_c c; char z; };
)";

// A main-file record instantiated inside the header's namespace, which only a
// walk of the preamble finds.
const char *nested = R"(#ifdef __cplusplus
namespace pt { template <class T> struct Box<T &> { T *ref; }; }
pt::Box<int &> refBox;
#endif
)";

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
  llvm::SmallString<128> dir, path;
  std::error_code ec = llvm::sys::fs::createUniqueDirectory("lense-preamble", dir);
  path = dir;
  llvm::sys::path::append(path, "lense_preamble_test.h");
  if (!ec) {
    llvm::raw_fd_ostream os(path, ec);
    os << header;
  }
  if (ec) {
    llvm::errs() << "cannot write " << path << ": " << ec.message() << "\n";
    return 2;
  }

  int failures = 0;
  for (const char *target : {"x86_64-linux-gnu", "x86_64-pc-windows-msvc"}) {
    lense::RunOptions opts;
    opts.clangArgs = {std::string("--target=") + target, "-I" + dir.str().str()};
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
      std::string records = "#include <stddef.h>\n"
                            "#include \"lense_preamble_test.h\"\n" +
                            std::string(uses) + code;
      std::string recordsEdited = records + tail;
      std::string recordsNested = records + nested;
      // A warning or an error in the preamble: neither may be cached.
      std::string warning = "#warning preamble\n" + code;
      std::string error = "#include \"lense_missing.h\"\n" + code;
      for (const std::string *v : {&code, &headers, &edited, &warning, &error,
                                   &records, &recordsEdited, &recordsNested}) {
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
          bool shouldReuse = v == &edited || v == &recordsEdited ||
                             v == &recordsNested ||
                             ((v == &headers || v == &records) && repeat);
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
  llvm::sys::fs::remove_directories(dir);
  return failures != 0;
}
