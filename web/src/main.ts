import { cpp } from '@codemirror/lang-cpp';
import { HighlightStyle, syntaxHighlighting } from '@codemirror/language';
import { type Diagnostic, lintGutter, setDiagnostics } from '@codemirror/lint';
import type { EditorState } from '@codemirror/state';
import { tags as t } from '@lezer/highlight';
import { basicSetup, EditorView } from 'codemirror';
import type { FromWorker, ToWorker } from './protocol';
import { render, tooltips } from './render';
import type { Options } from './types';
import './style.css';

const SAMPLE = `// Edit the code: the layouts update as you type.
struct Shape {
  virtual ~Shape();
  virtual double area() const = 0;
  int id;
};

struct Circle : Shape {
  double area() const override;
  char tag;
  double radius;
};

struct Flags {
  unsigned ready : 1;
  unsigned mode : 3;
  unsigned short count;
};

struct Stream { virtual void read(); long pos; };
struct In : virtual Stream { int fd; };
struct Out : virtual Stream { int fd; };
struct InOut : In, Out { void read() override; };
`;

const STANDARDS: Record<Options['lang'], string[]> = {
  'c++': ['c++11', 'c++14', 'c++17', 'c++20', 'c++23', 'c++2c'],
  c: ['c89', 'c99', 'c11', 'c17', 'c23'],
};

interface Saved {
  code: string;
  lang: Options['lang'];
  std: string;
  target: string;
  vtables: boolean;
  expand: boolean;
  flags: string;
}

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const langBox = $<HTMLSelectElement>('lang');
const stdBox = $<HTMLSelectElement>('std');
const targetBox = $<HTMLSelectElement>('target');
const vtablesBox = $<HTMLInputElement>('vtables');
const expandBox = $<HTMLInputElement>('expand');
const flagsBox = $<HTMLInputElement>('flags');
const statusBox = $<HTMLSpanElement>('status');
const output = $<HTMLElement>('output');

// --- Saving the state in the URL -------------------------------------------

function toB64(b: Uint8Array): string {
  let s = '';
  for (const c of b) s += String.fromCharCode(c);
  return btoa(s).replaceAll('+', '-').replaceAll('/', '_').replace(/=+$/, '');
}
const fromB64 = (s: string) =>
  Uint8Array.from(atob(s.replaceAll('-', '+').replaceAll('_', '/')), (c) => c.charCodeAt(0));

async function pack(s: Saved): Promise<string> {
  const stream = new Blob([JSON.stringify(s)]).stream().pipeThrough(new CompressionStream('deflate-raw'));
  return toB64(new Uint8Array(await new Response(stream).arrayBuffer()));
}

async function unpack(hash: string): Promise<Saved | null> {
  try {
    const stream = new Blob([fromB64(hash)]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    return JSON.parse(await new Response(stream).text());
  } catch {
    return null;
  }
}

function fillStandards(lang: Options['lang'], value: string) {
  stdBox.replaceChildren(new Option('default', ''),
                         ...STANDARDS[lang].map((s) => new Option(s, s)));
  stdBox.value = STANDARDS[lang].includes(value) ? value : '';
}

function current(): Saved {
  return {
    code: view.state.doc.toString(),
    lang: langBox.value as Options['lang'],
    std: stdBox.value,
    target: targetBox.value,
    vtables: vtablesBox.checked,
    expand: expandBox.checked,
    flags: flagsBox.value,
  };
}

function options(s: Saved): Options {
  const args = s.flags.split(/\s+/).filter(Boolean);
  if (s.std) args.unshift(`-std=${s.std}`);
  return { lang: s.lang, target: s.target, vtables: s.vtables, expandSystem: s.expand, args };
}

// --- Diagnostics ----------------------------------------------------------

const DIAG = /^(.*?):(\d+):(\d+): (fatal error|error|warning|note): (.*)$/;
const INCLUDED = /^In file included from input\.(?:c|cpp):(\d+):/;

/** Turns clang's stderr into editor diagnostics. Errors inside headers are
 *  shown on the #include line that led to them. */
function diagnostics(state: EditorState, stderr: string): Diagnostic[] {
  const out: Diagnostic[] = [];
  let include = 0;
  const at = (line: number, col: number) => {
    const l = state.doc.line(Math.min(Math.max(line, 1), state.doc.lines));
    const from = Math.min(l.from + Math.max(col - 1, 0), l.to);
    const word = state.wordAt(from);
    return word ? { from: word.from, to: word.to } : { from, to: Math.min(from + 1, l.to) };
  };
  for (const line of stderr.split('\n')) {
    const inc = INCLUDED.exec(line);
    if (inc) {
      include = Number(inc[1]);
      continue;
    }
    const m = DIAG.exec(line);
    if (!m) continue;
    const [, file, l, c, kind, message] = m;
    const severity = kind === 'warning' ? 'warning' : kind === 'note' ? 'info' : 'error';
    if (/^input\.(c|cpp)$/.test(file)) {
      include = 0;
      out.push({ ...at(Number(l), Number(c)), severity, message, source: 'clang' });
    } else if (include && severity !== 'info') {
      const name = file.replace(/^\/lense\/(include\/(c\+\+\/v1\/)?|clang\/include\/)/, '');
      out.push({ ...at(include, 1), severity, message: `${name}:${l}: ${message}`, source: 'clang' });
    }
  }
  return out;
}

// --- Editor ---------------------------------------------------------------

const highlight = HighlightStyle.define([
  { tag: [t.keyword, t.controlKeyword, t.modifier, t.operatorKeyword], color: 'var(--syn-keyword)' },
  { tag: [t.typeName, t.standard(t.typeName)], color: 'var(--syn-type)' },
  { tag: [t.string, t.character], color: 'var(--syn-string)' },
  { tag: [t.number, t.bool, t.null], color: 'var(--syn-number)' },
  { tag: [t.comment, t.lineComment, t.blockComment], color: 'var(--syn-comment)', fontStyle: 'italic' },
  { tag: [t.processingInstruction, t.macroName], color: 'var(--syn-preproc)' },
  { tag: [t.definition(t.variableName), t.function(t.variableName)], color: 'var(--ink)' },
]);

const theme = EditorView.theme({
  '&': { height: '100%', backgroundColor: 'var(--surface)', color: 'var(--ink)' },
  '.cm-scroller': { fontFamily: 'var(--mono)', fontSize: '13.5px', lineHeight: '1.55' },
  '.cm-gutters': { backgroundColor: 'var(--surface)', color: 'var(--muted)', border: 'none' },
  '.cm-activeLine, .cm-activeLineGutter': { backgroundColor: 'var(--active-line)' },
  '.cm-cursor': { borderLeftColor: 'var(--ink)' },
  '&.cm-focused .cm-selectionBackground, .cm-selectionBackground, ::selection':
    { backgroundColor: 'var(--selection) !important' },
  '.cm-tooltip': { backgroundColor: 'var(--surface)', border: '1px solid var(--border)', color: 'var(--ink)' },
});

let timer = 0;
const view = new EditorView({
  parent: $('editor'),
  extensions: [
    basicSetup, cpp(), syntaxHighlighting(highlight), theme, lintGutter(),
    EditorView.updateListener.of((u) => {
      if (!u.docChanged) return;
      clearTimeout(timer);
      timer = setTimeout(analyze, 250);
    }),
  ],
});

function locate(line: number, column: number) {
  const l = view.state.doc.line(Math.min(line, view.state.doc.lines));
  const pos = Math.min(l.from + column - 1, l.to);
  view.dispatch({ selection: { anchor: pos }, effects: EditorView.scrollIntoView(pos, { y: 'center' }) });
  view.focus();
}

// --- Worker ---------------------------------------------------------------

const worker = new Worker(new URL('./worker.ts', import.meta.url), { type: 'module' });
let ready = false, busy = false, seq = 0;
let queued: ToWorker | null = null;

function send(msg: ToWorker) {
  if (!ready || busy) {
    queued = msg;
    return;
  }
  busy = true;
  worker.postMessage(msg);
}

async function analyze() {
  const s = current();
  send({ type: 'analyze', id: ++seq, code: s.code, options: options(s) });
  history.replaceState(null, '', `#${await pack(s)}`);
}

function status(text: string, error = false) {
  statusBox.textContent = text;
  statusBox.classList.toggle('error', error);
}

worker.onmessage = (ev: MessageEvent<FromWorker>) => {
  const m = ev.data;
  switch (m.type) {
    case 'status': {
      const p = /\((\d+)\/(\d+)\)/.exec(m.text);
      status(p ? `Loading headers ${(+p[1] / 1e6).toFixed(1)} / ${(+p[2] / 1e6).toFixed(1)} MB`
               : 'Loading clang');
      return;
    }
    case 'ready':
      ready = true;
      status(`clang ready in ${Math.round(m.ms)} ms`);
      if (queued) {
        const q = queued;
        queued = null;
        send(q);
      }
      return;
    case 'fatal':
      status(`Could not load clang: ${m.message}`, true);
      return;
    case 'result': {
      busy = false;
      if (queued) {
        const q = queued;
        queued = null;
        send(q);
      }
      if (m.id !== seq) return;
      view.dispatch(setDiagnostics(view.state, diagnostics(view.state, m.stderr)));
      output.replaceChildren();
      const r = m.result;
      if ('error' in r) {
        output.append(Object.assign(document.createElement('p'), { className: 'failure', textContent: r.error }));
        status('lense failed', true);
      } else {
        if (!r.ok) {
          output.append(Object.assign(document.createElement('p'), {
            className: 'failure',
            textContent: 'clang reported errors. The records it could lay out are below.',
          }));
        }
        render(output, r.report, { locate });
        status(`${r.ok ? '' : 'errors · '}${Math.round(m.ms)} ms`, !r.ok);
      }
      if (m.stderr.trim()) {
        const d = document.createElement('details');
        d.className = 'stderr';
        d.open = !('error' in r) && !r.ok && !r.report.records.length;
        d.append(Object.assign(document.createElement('summary'), { textContent: 'clang output' }),
                 Object.assign(document.createElement('pre'), { textContent: m.stderr }));
        output.append(d);
      }
    }
  }
};

// --- Start ----------------------------------------------------------------

async function start() {
  const saved = (location.hash.length > 1 && (await unpack(location.hash.slice(1)))) || null;
  const s: Saved = saved ?? {
    code: SAMPLE, lang: 'c++', std: '', target: 'x86_64-linux-gnu',
    vtables: true, expand: false, flags: '',
  };
  view.dispatch({ changes: { from: 0, to: view.state.doc.length, insert: s.code } });
  clearTimeout(timer);
  langBox.value = s.lang;
  fillStandards(s.lang, s.std);
  targetBox.value = s.target;
  if (targetBox.value !== s.target) targetBox.append(new Option(s.target, s.target, true, true));
  vtablesBox.checked = s.vtables;
  expandBox.checked = s.expand;
  flagsBox.value = s.flags;

  langBox.onchange = () => {
    fillStandards(langBox.value as Options['lang'], '');
    analyze();
  };
  for (const box of [stdBox, targetBox, vtablesBox, expandBox]) box.onchange = analyze;
  flagsBox.oninput = () => {
    clearTimeout(timer);
    timer = setTimeout(analyze, 400);
  };
  tooltips(output, $('tip'));

  status('Loading clang');
  worker.postMessage({ type: 'init', url: new URL('lense/lense-web.js', document.baseURI).href } satisfies ToWorker);
  analyze();
}

start();
