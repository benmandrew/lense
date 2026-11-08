// The clang module (lense-web.{js,wasm,data}) comes from the Emscripten build
// of lense. Set LENSE_WASM_DIR to use a build other than ../build-wasm.
import { existsSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { defineConfig, type Plugin } from 'vite';

const dir = resolve(import.meta.dirname, process.env.LENSE_WASM_DIR ?? '../build-wasm/lense/wasm');
const files: Record<string, string> = {
  'lense-web.js': 'text/javascript',
  'lense-web.wasm': 'application/wasm',
  'lense-web.data': 'application/octet-stream',
};

function read(name: string): Buffer {
  const path = resolve(dir, name);
  if (!existsSync(path))
    throw new Error(`${path} is missing: build lense_web with Emscripten or set LENSE_WASM_DIR`);
  return readFileSync(path);
}

// Serves the module at lense/ in development and copies it into the build.
// Vite must not bundle it: Emscripten finds the .wasm and .data files next
// to its own URL.
function lenseModule(): Plugin {
  return {
    name: 'lense-module',
    configureServer(server) {
      server.middlewares.use('/lense', (req, res, next) => {
        const name = (req.url ?? '').split('?')[0].replace(/^\//, '');
        if (!(name in files)) return next();
        res.setHeader('Content-Type', files[name]);
        res.end(read(name));
      });
    },
    generateBundle() {
      for (const name of Object.keys(files))
        this.emitFile({ type: 'asset', fileName: `lense/${name}`, source: read(name) });
    },
  };
}

export default defineConfig({
  base: './',
  plugins: [lenseModule()],
  worker: { format: 'es' },
  // CodeMirror with the C++ grammar is about 530 kB.
  build: { target: 'es2022', chunkSizeWarningLimit: 700 },
});
