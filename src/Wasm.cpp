// Entry point of the browser build (lense-web.js). JavaScript calls
// lense_analyze once per analysis; clang's diagnostics go to stderr, which
// the caller captures.
#include "Json.h"
#include "Run.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <emscripten/emscripten.h>

#include <cstdlib>
#include <cstring>

namespace json = llvm::json;

namespace {

char *copy(const std::string &s) {
  char *p = static_cast<char *>(std::malloc(s.size() + 1));
  std::memcpy(p, s.c_str(), s.size() + 1);
  return p;
}

char *error(const std::string &message) {
  std::string s;
  llvm::raw_string_ostream os(s);
  os << json::Value(json::Object{{"error", message}});
  return copy(s);
}

} // namespace

// `options` is a JSON object, every field optional:
//   {"lang": "c" | "c++", "target": "<triple>", "expandSystem": false,
//    "records": ["<name>", ...],
//    "args": ["<clang argument>", ...], "cacheLine": 64, "reorder": true,
//    "compare": "<triple>"}
// Returns {"ok": <no clang errors>, "report": <as lense --json>} or
// {"error": "<message>"}, as a string the caller must free(). With
// "compare", the layout for that target is in "compare".
extern "C" EMSCRIPTEN_KEEPALIVE char *lense_analyze(const char *code,
                                                    const char *options) {
  llvm::Expected<json::Value> parsed = json::parse(options);
  if (!parsed)
    return error("bad options: " + llvm::toString(parsed.takeError()));
  const json::Object *o = parsed->getAsObject();
  if (!o)
    return error("options must be a JSON object");

  lense::RunOptions opts;
  std::string lang = o->getString("lang").value_or("c++").str();
  if (lang != "c" && lang != "c++")
    return error("lang must be c or c++");
  opts.analyze.expandSystem = o->getBoolean("expandSystem").value_or(false);
  opts.analyze.reorder = o->getBoolean("reorder").value_or(true);
  int64_t cacheLine = o->getInteger("cacheLine").value_or(64);
  if (cacheLine < 0 || cacheLine > (1 << 20) || (cacheLine & (cacheLine - 1)))
    return error("cacheLine must be a power of two or 0");
  opts.analyze.cacheLine = unsigned(cacheLine);
  std::string compare = o->getString("compare").value_or("").str();
  if (const json::Array *records = o->getArray("records"))
    for (const json::Value &r : *records)
      if (auto name = r.getAsString())
        opts.analyze.records.push_back(name->str());
  if (auto target = o->getString("target"); target && !target->empty())
    opts.clangArgs.push_back("--target=" + target->str());
  if (const json::Array *args = o->getArray("args"))
    for (const json::Value &a : *args)
      if (auto arg = a.getAsString())
        opts.clangArgs.push_back(arg->str());

  const char *file = lang == "c" ? "input.c" : "input.cpp";
  lense::Report report, other;
  bool ok = lense::run(code, file, opts, report);
  if (!compare.empty()) {
    opts.clangArgs.push_back("--target=" + compare);
    ok &= lense::run(code, file, opts, other);
  }
  std::string s;
  llvm::raw_string_ostream os(s);
  os << "{\"ok\":" << (ok ? "true" : "false") << ",\"report\":";
  lense::printJson(os, report);
  if (!compare.empty()) {
    os << ",\"compare\":";
    lense::printJson(os, other);
  }
  os << "}";
  return copy(s);
}
