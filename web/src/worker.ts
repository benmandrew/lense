/// <reference lib="webworker" />
// Runs lense-web.js (clang compiled to WebAssembly) off the main thread.
import type { FromWorker, ToWorker } from './protocol';
import type { AnalyzeResult } from './types';

interface LenseModule {
  ccall(name: string, ret: 'number', types: string[], args: unknown[]): number;
  UTF8ToString(ptr: number): string;
  _free(ptr: number): void;
}

let url = '';
let module: Promise<LenseModule> | null = null;
let stderr: string[] = [];

const post = (m: FromWorker) => postMessage(m);

function load(): Promise<LenseModule> {
  module ??= import(/* @vite-ignore */ url).then((m) =>
    m.default({
      // Emscripten looks for the .data package next to the page (or worker)
      // unless told otherwise; keep it next to lense-web.js.
      locateFile: (path: string) => new URL(path, url).href,
      print: () => {},
      printErr: (line: string) => stderr.push(line),
      // Emscripten reports download progress of the header package here.
      setStatus: (text: string) => text && post({ type: 'status', text }),
    }),
  );
  return module;
}

onmessage = async (e: MessageEvent<ToWorker>) => {
  const msg = e.data;
  const start = performance.now();
  if (msg.type === 'init') {
    url = msg.url;
    try {
      await load();
      post({ type: 'ready', ms: performance.now() - start });
    } catch (err) {
      module = null;
      post({ type: 'fatal', message: String(err) });
    }
    return;
  }
  stderr = [];
  let result: AnalyzeResult;
  try {
    const m = await load();
    const ptr = m.ccall('lense_analyze', 'number', ['string', 'string'],
                        [msg.code, JSON.stringify(msg.options)]);
    const text = m.UTF8ToString(ptr);
    m._free(ptr);
    result = JSON.parse(text);
  } catch (err) {
    // A trap (for example an LLVM fatal error) leaves the module unusable,
    // so start a fresh one for the next request.
    module = null;
    result = { error: `lense stopped: ${err}` };
  }
  post({ type: 'result', id: msg.id, result, stderr: stderr.join('\n'),
         ms: performance.now() - start });
};
