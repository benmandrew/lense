# Plan

## Done (native CLI, 2026-11-08)

- LibTooling core: record layout tree, padding holes, Itanium vtable groups with address points and thunks, Microsoft vftables, vbptr and vtordisp.
- Text and JSON printers, `--target`, `--record`, `--no-vtables`, `--expand-system`, stdin input.
- Golden tests: 9 cases × (Itanium JSON, MSVC JSON, text) = 27 tests.
- Nix flake for all dependencies (LLVM 23.1.3, GCC 16.2 as advisory oracle); `nix build` runs the full suite.
- E2E CLI tests (20) and fuzzing: runtime oracle, static cross-target checks, invariants, mutation robustness, shrinking. 1,500 oracle + 2,000 mutation cases clean.

## Next: CLI gaps

- [ ] Expand arrays of records (show element padding), perhaps behind a flag.
- [ ] Microsoft vbtable contents (`MicrosoftVTableContext::getVBTableIndex`, vbase offsets).
- [ ] Itanium VTT and construction vtables for classes with virtual bases.
- [ ] Capture diagnostics into the report (custom `DiagnosticConsumer` via `ToolInvocation`) so JSON consumers see errors. Needed for the browser.
- [ ] `--hex` offsets; a compact one-line-per-record summary mode.
- [ ] Linux: the flake lists x86_64/aarch64-linux but only aarch64-darwin has been built.
- [ ] Fuzzer coverage: templates, anonymous members, covariant returns, pure virtuals (needs non-instantiated abstract classes), MSVC runtime checks (no Windows runner).
- [ ] Optionally match Apple clang: investigate `-fclang-abi-compat` for packed non-POD members.

## Then: browser

- [ ] Cross-compile the clang libraries for wasm32 with Emscripten. Needs a native `clang-tblgen`/`llvm-tblgen` first; no LLVM targets (`LLVM_TARGETS_TO_BUILD=""`), Clang AST/Sema/Frontend/Tooling only.
- [ ] Wasm entry point: `run()` on a string → JSON, with an in-memory VFS (`runToolOnCodeWithArgs` VFS overload) holding Emscripten's libc++/libc headers and clang's resource headers.
- [ ] Measure the `.wasm` download size; strip and compress.
- [ ] Front end: TypeScript + Vite, CodeMirror 6 editor on the left, DOM/SVG layout view on the right, target picker. Run the analysis in a Web Worker on each edit (debounced).
