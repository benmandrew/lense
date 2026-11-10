import { cpp } from '@codemirror/lang-cpp';
import { HighlightStyle, syntaxHighlighting } from '@codemirror/language';
import { type Diagnostic, lintGutter, setDiagnostics } from '@codemirror/lint';
import { type EditorState, StateEffect, StateField, type Text } from '@codemirror/state';
import { Decoration, type DecorationSet } from '@codemirror/view';
import { tags as t } from '@lezer/highlight';
import { basicSetup, EditorView } from 'codemirror';
import EXAMPLES from 'virtual:examples';
import type { FromWorker, ToWorker } from './protocol';
import { el, render, tooltips } from './render';
import type { Options, SourceRange } from './types';
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
  /** A second target to compare with; empty is off. */
  compare: string;
  /** Bytes; 0 is off. */
  cacheLine: number;
  expand: boolean;
  flags: string;
}

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const exampleBox = $<HTMLSelectElement>('example');
const langBox = $<HTMLSelectElement>('lang');
const stdBox = $<HTMLSelectElement>('std');
const targetBox = $<HTMLSelectElement>('target');
const compareBox = $<HTMLSelectElement>('compare');
const cacheLineBox = $<HTMLSelectElement>('cacheline');
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

const through = (data: BlobPart, stream: CompressionStream | DecompressionStream) =>
  new Response(new Blob([data]).stream().pipeThrough(stream));

async function pack(s: Saved): Promise<string> {
  const deflated = through(JSON.stringify(s), new CompressionStream('deflate-raw'));
  return toB64(new Uint8Array(await deflated.arrayBuffer()));
}

async function unpack(hash: string): Promise<Saved | null> {
  try {
    return JSON.parse(await through(fromB64(hash), new DecompressionStream('deflate-raw')).text());
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
    compare: compareBox.value,
    cacheLine: Number(cacheLineBox.value),
    expand: expandBox.checked,
    flags: flagsBox.value,
  };
}

function options(s: Saved): Options {
  const args = s.flags.split(/\s+/).filter(Boolean);
  if (s.std) args.unshift(`-std=${s.std}`);
  return {
    lang: s.lang, target: s.target, expandSystem: s.expand, args,
    cacheLine: s.cacheLine, ranges: true, ...(s.compare ? { compare: s.compare } : {}),
  };
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
  // A rule also covers the tags derived from its own, such as
  // t.controlKeyword from t.keyword. t.null is a keyword, so it is named.
  { tag: t.keyword, color: 'var(--syn-keyword)' },
  { tag: t.typeName, color: 'var(--syn-type)' },
  { tag: t.string, color: 'var(--syn-string)' },
  { tag: [t.number, t.bool, t.null], color: 'var(--syn-number)' },
  { tag: t.comment, color: 'var(--syn-comment)', fontStyle: 'italic' },
  { tag: [t.processingInstruction, t.macroName], color: 'var(--syn-preproc)' },
  { tag: [t.definition(t.variableName), t.function(t.variableName), t.function(t.propertyName)],
    color: 'var(--syn-function)' },
  { tag: [t.variableName, t.propertyName], color: 'var(--syn-variable)' },
  { tag: [t.operator, t.punctuation], color: 'var(--syn-operator)' },
]);

const theme = EditorView.theme({
  '&': { height: '100%', backgroundColor: 'var(--code-bg)', color: 'var(--ink-2)' },
  '.cm-scroller': { fontFamily: 'var(--mono)', fontSize: '13.5px', lineHeight: '1.55' },
  '.cm-gutters': { backgroundColor: 'var(--code-bg)', color: 'var(--muted)', border: 'none' },
  '.cm-activeLine, .cm-activeLineGutter': { backgroundColor: 'var(--active-line)' },
  '.cm-cursor': { borderLeftColor: 'var(--ink)' },
  '&.cm-focused .cm-selectionBackground, .cm-selectionBackground, ::selection':
    { backgroundColor: 'var(--selection) !important' },
  '.cm-tooltip': { backgroundColor: 'var(--surface)', border: '1px solid var(--border)', color: 'var(--ink-2)' },
  // The selection colour, which every syntax colour reaches 4.5:1 on. It is
  // close to --code-bg, so an underline carries the mark.
  '.cm-linked': { backgroundColor: 'var(--hl-bg)', boxShadow: 'inset 0 -1.5px var(--muted)' },
}, { dark: true });

// The declarations of the member under the pointer in the layouts.
const setLinked = StateEffect.define<{ from: number; to: number }[]>();
const linkedMark = Decoration.mark({ class: 'cm-linked' });
const linked = StateField.define<DecorationSet>({
  create: () => Decoration.none,
  update(set, tr) {
    set = set.map(tr.changes);
    for (const e of tr.effects) {
      if (e.is(setLinked))
        set = Decoration.set(e.value.map((r) => linkedMark.range(r.from, r.to)), true);
    }
    return set;
  },
  provide: (f) => EditorView.decorations.from(f),
});

let timer = 0;
const view = new EditorView({
  parent: $('editor'),
  extensions: [
    basicSetup, cpp(), syntaxHighlighting(highlight), theme, lintGutter(), linked,
    EditorView.updateListener.of((u) => {
      if (!u.docChanged) return;
      clearTimeout(timer);
      timer = setTimeout(analyze, 250);
    }),
  ],
});

/** A line and column from clang, both from 1, as a document position.
 *  clang counts columns in UTF-8 bytes and CodeMirror in UTF-16 units. */
function position(doc: Text, line: number, column: number): number {
  const l = doc.line(Math.min(Math.max(line, 1), doc.lines));
  const bytes = new TextEncoder().encode(l.text).subarray(0, Math.max(column - 1, 0));
  return l.from + Math.min(new TextDecoder().decode(bytes).length, l.length);
}

function locate(line: number, column: number) {
  const pos = position(view.state.doc, line, column);
  view.dispatch({ selection: { anchor: pos }, effects: EditorView.scrollIntoView(pos, { y: 'center' }) });
  view.focus();
}

function mark(ranges: SourceRange[]) {
  const doc = view.state.doc;
  const spans = ranges
    .map((r) => ({ from: position(doc, r.line, r.column), to: position(doc, r.end_line, r.end_column) }))
    .filter((s) => s.from < s.to);
  if (spans.length || view.state.field(linked).size)
    view.dispatch({ effects: setLinked.of(spans) });
}

// --- Worker ---------------------------------------------------------------

const worker = new Worker(new URL('./worker.ts', import.meta.url), { type: 'module' });
let ready = false, busy = false, seq = 0;
let queued: ToWorker | null = null;

/** Posts msg, or the one held back, once the worker is free. A newer
 *  message replaces a held one. */
function send(msg?: ToWorker) {
  if (msg) queued = msg;
  if (!ready || busy || !queued) return;
  busy = true;
  worker.postMessage(queued);
  queued = null;
}

const examples = [{ name: 'sample', lang: 'c++' as const, code: SAMPLE }, ...EXAMPLES];

/** Selects the example the editor holds, or "edited" if it holds none. */
function syncExample(code: string) {
  exampleBox.value = examples.find((e) => e.code === code)?.name ?? '';
}

async function analyze() {
  const s = current();
  syncExample(s.code);
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
      send();
      return;
    case 'fatal':
      status(`Could not load clang: ${m.message}`, true);
      return;
    case 'result': {
      busy = false;
      send();
      if (m.id !== seq) return;
      view.dispatch(setDiagnostics(view.state, diagnostics(view.state, m.stderr)));
      // The element under the pointer goes, so its pointerleave never fires.
      mark([]);
      output.replaceChildren();
      const r = m.result;
      if ('error' in r) {
        output.append(el('p', 'failure', r.error));
        status('lense failed', true);
      } else {
        if (!r.ok) {
          output.append(el('p', 'failure',
                           'clang reported errors. The records it could lay out are below.'));
        }
        render(output, r.report, { locate, mark }, r.compare);
        status(`${r.ok ? '' : 'errors · '}${Math.round(m.ms)} ms`, !r.ok);
      }
      if (m.stderr.trim()) {
        const d = el('details', 'stderr');
        d.open = !('error' in r) && !r.ok && !r.report.records.length;
        d.append(el('summary', '', 'clang output'), el('pre', '', m.stderr));
        output.append(d);
      }
    }
  }
};

// --- Start ----------------------------------------------------------------

/** Replaces the editor text without the typing delay; callers analyze at once. */
function setCode(code: string) {
  view.dispatch({ changes: { from: 0, to: view.state.doc.length, insert: code } });
  clearTimeout(timer);
}

/** Selects value, adding it first if a saved link names one the page lacks. */
function choose(box: HTMLSelectElement, value: string, text = value) {
  box.value = value;
  if (box.value !== value) box.append(new Option(text, value, true, true));
}

async function start() {
  const saved = (location.hash.length > 1 && (await unpack(location.hash.slice(1)))) || null;
  // Links saved before an option existed leave it out.
  const s: Saved = {
    code: SAMPLE, lang: 'c++', std: '', target: 'x86_64-linux-gnu', compare: '', cacheLine: 64,
    expand: false, flags: '', ...saved,
  };
  setCode(s.code);
  langBox.value = s.lang;
  fillStandards(s.lang, s.std);
  choose(targetBox, s.target);
  compareBox.append(...[...targetBox.options].map((o) => new Option(o.text, o.value)));
  choose(compareBox, s.compare);
  choose(cacheLineBox, String(s.cacheLine), `${s.cacheLine} bytes`);
  expandBox.checked = s.expand;
  flagsBox.value = s.flags;

  const edited = new Option('edited', '');
  edited.hidden = true;
  exampleBox.append(edited, ...examples.map((e) => new Option(e.name, e.name)));
  syncExample(s.code);
  exampleBox.onchange = () => {
    const e = examples.find((x) => x.name === exampleBox.value);
    if (!e) return;
    if (langBox.value !== e.lang) {
      langBox.value = e.lang;
      fillStandards(e.lang, '');
    }
    setCode(e.code);
    analyze();
  };

  langBox.onchange = () => {
    fillStandards(langBox.value as Options['lang'], '');
    analyze();
  };
  for (const box of [stdBox, targetBox, compareBox, cacheLineBox, expandBox]) box.onchange = analyze;
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
