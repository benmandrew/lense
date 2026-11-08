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
//   {"lang": "c" | "c++", "target": "<triple>", "vtables": true,
//    "expandSystem": false, "records": ["<name>", ...],
//    "args": ["<clang argument>", ...]}
// Returns {"ok": <no clang errors>, "report": <as lense --json>} or
// {"error": "<message>"}, as a string the caller must free().
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
  opts.analyze.vtables = o->getBoolean("vtables").value_or(true);
  opts.analyze.expandSystem = o->getBoolean("expandSystem").value_or(false);
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

  lense::Report report;
  bool ok = lense::run(code, lang == "c" ? "input.c" : "input.cpp", opts,
                       report);
  std::string s;
  llvm::raw_string_ostream os(s);
  os << "{\"ok\":" << (ok ? "true" : "false") << ",\"report\":";
  lense::printJson(os, report);
  os << "}";
  return copy(s);
}
