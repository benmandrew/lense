// Draws a lense report: per record, a byte map, the member table (worded
// like the CLI's text output) and the vtables. With a second report, draws
// the two targets side by side instead, as lense --compare does.
import {
  bits, type Entry, type LayoutNode, type LayoutRecord, type Report, type Thunk, type VTable,
} from './types';

export interface Hooks {
  locate(line: number, column: number): void;
}

type Category = 'field' | 'base' | 'ptr' | 'pad';
const CATEGORIES: [Category, string][] = [
  ['field', 'member'],
  ['base', 'inherited member'],
  ['ptr', 'vptr, vbptr or vtordisp'],
  ['pad', 'padding'],
];
const BASE_ROLES = new Set(['primary_base', 'base', 'primary_virtual_base', 'virtual_base']);

interface Leaf {
  node: LayoutNode;
  path: string;
  cat: Category;
  start: number;
  end: number;
}

function el<K extends keyof HTMLElementTagNameMap>(
  tag: K, cls?: string, text?: string,
): HTMLElementTagNameMap[K] {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

function span(parent: HTMLElement, cls: string, text: string): HTMLSpanElement {
  const s = el('span', cls, text);
  parent.append(s);
  return s;
}

const plural = (n: number, one: string, many = `${one}s`) => `${n} ${n === 1 ? one : many}`;
const fmtOffset = (b: number) => (b % 8 ? `${Math.floor(b / 8)}:${b % 8}` : `${b / 8}`);
const fmtSize = (b: number, inBits: boolean) => (!inBits && b % 8 === 0 ? `${b / 8}` : `${b}b`);
const fmtAmount = (b: number) => (b % 8 === 0 ? plural(b / 8, 'byte') : `${b} bits`);
const signed = (n: number) => (n < 0 ? `-${-n}` : `+${n}`);

/** Sizes of bitfields, and of padding that is not whole bytes, are in bits. */
function fmtSizeOf(n: LayoutNode): string {
  const [start, end] = bits(n);
  const inBits = n.kind === 'bitfield' ||
    (n.kind === 'padding' && (start % 8 !== 0 || end % 8 !== 0));
  return fmtSize(end - start, inBits);
}

const fmtAlign = (n: LayoutNode) =>
  (!n.align || n.kind === 'bitfield' || n.kind === 'padding' ? '' : String(n.align));

/** Only records with a bitfield say which byte order they assume. */
const hasBitFields = (nodes: LayoutNode[]): boolean =>
  nodes.some((n) => n.memory_location !== undefined || hasBitFields(n.members ?? []));

function category(n: LayoutNode, inBase: boolean): Category {
  if (n.kind === 'vptr' || n.kind === 'vbptr' || n.kind === 'vtordisp') return 'ptr';
  if (n.kind === 'padding') return 'pad';
  return inBase ? 'base' : 'field';
}

/** The nodes that occupy storage, in order, with their position in the tree. */
function leaves(nodes: LayoutNode[], prefix: string, inBase: boolean, out: Leaf[]): Leaf[] {
  nodes.forEach((n, i) => {
    const path = prefix ? `${prefix}.${i}` : `${i}`;
    const base = inBase || BASE_ROLES.has(n.role ?? '');
    const [start, end] = bits(n);
    if (n.kind === 'record' && n.members?.length) leaves(n.members, path, base, out);
    else if (end > start) out.push({ node: n, path, cat: category(n, base), start, end });
  });
  return out;
}

function label(n: LayoutNode, ms: boolean): string {
  switch (n.kind) {
    case 'vptr': return ms ? 'vfptr' : 'vptr';
    case 'vbptr': return 'vbptr';
    case 'vtordisp': return 'vtordisp';
    case 'padding': return '';
    default: return n.name || n.type || '(unnamed)';
  }
}

/** One member line, worded like src/Text.cpp. */
function describe(parent: HTMLElement, n: LayoutNode, ms: boolean, link?: HTMLElement) {
  switch (n.kind) {
    case 'record':
      if (n.role === 'field' || !n.role) {
        span(parent, 'type', n.type ?? '');
        if (n.name) parent.append(` ${n.name}`);
      } else {
        span(parent, 'base', n.type ?? '');
        span(parent, 'dim', `  ${n.role.replaceAll('_', ' ')}`);
      }
      if (n.empty) span(parent, 'dim', ' (empty)');
      break;
    case 'field':
    case 'bitfield':
      span(parent, 'type', n.type ?? '');
      parent.append(` ${n.name || '(unnamed)'}`);
      if (n.kind === 'bitfield') parent.append(` : ${n.bit_size}`);
      break;
    case 'vptr':
      span(parent, 'ptr', ms ? 'vfptr' : 'vptr');
      span(parent, 'dim', `  ${n.type ?? ''}`);
      if (n.points_to) {
        parent.append('  ');
        parent.append(link ?? `-> ${n.points_to.table}[${n.points_to.slot}]`);
      }
      break;
    case 'vbptr':
      span(parent, 'ptr', 'vbptr');
      span(parent, 'dim', `  ${n.type ?? ''}`);
      break;
    case 'vtordisp':
      span(parent, 'ptr', 'vtordisp');
      span(parent, 'dim', `  for ${n.type ?? ''}`);
      break;
    case 'padding':
      span(parent, 'pad', 'padding');
      break;
  }
}

/** The notes after a member line, as src/Text.cpp prints them. */
function notes(parent: HTMLElement, n: LayoutNode) {
  if (n.memory_location) {
    span(parent, 'dim', `  location ${n.memory_location}`).title =
      'Memory location: two threads may write bitfields in different locations at once';
  }
  if (n.unaligned) {
    span(parent, 'chip bad', 'unaligned').title =
      'Its offset is not a multiple of the alignment of its type';
  }
  if (n.crosses_cache_line) {
    span(parent, 'chip warn', 'crosses cache line').title =
      'Starts in one cache line and ends in another';
  }
}

function tip(n: LayoutNode, ms: boolean): string {
  const s = el('span');
  describe(s, n, ms);
  const [start, end] = bits(n);
  const inBits = n.kind === 'bitfield' || start % 8 !== 0 || end % 8 !== 0;
  let where = inBits
    ? `bits ${start}–${end - 1} (byte ${fmtOffset(start)}), ${plural(end - start, 'bit')}`
    : `offset ${start / 8}, ${plural((end - start) / 8, 'byte')}`;
  if (fmtAlign(n)) where += `, align ${n.align}`;
  if (n.memory_location) where += `, memory location ${n.memory_location}`;
  const flags = [n.unaligned && 'unaligned', n.crosses_cache_line && 'crosses cache line'];
  const extra = flags.filter(Boolean).join(', ');
  return `${s.textContent?.replace(/\s+/g, ' ').trim()}\n${where}${extra ? `\n${extra}` : ''}`;
}

// --- Byte map -------------------------------------------------------------

interface Piece { leaf: Leaf; s: number; e: number }

function byteMap(r: LayoutRecord, all: Leaf[], ms: boolean, cacheLine = 0): HTMLElement {
  const bytes = Math.min(8, Math.max(1, r.size));
  const rowBits = bytes * 8;
  const rows = Math.ceil((r.size * 8) / rowBits);
  const map = el('div', 'bytemap');
  map.style.setProperty('--bits', String(rowBits));
  map.style.setProperty('--bytes', String(bytes));

  const ruler = el('div', 'row ruler');
  ruler.append(el('span', 'off'));
  const marks = el('div', 'marks');
  for (let b = 0; b < bytes; b++) marks.append(el('span', '', `+${b}`));
  ruler.append(marks);
  map.append(ruler);

  // Rows wholly inside one long member are folded, so a large array costs a
  // few rows. Every row where some member starts or ends is drawn.
  const shown = new Set<number>([0, rows - 1]);
  for (const l of all) {
    const a = Math.floor(l.start / rowBits), b = Math.floor((l.end - 1) / rowBits);
    for (const row of [a, a + 1, b - 1, b]) if (row >= a && row <= b) shown.add(row);
  }
  const order = [...shown].sort((x, y) => x - y);

  let prev = -1;
  for (const row of order) {
    if (row > prev + 1) {
      const skipped = row - prev - 1;
      const over = all.find((l) => l.start <= (prev + 1) * rowBits && l.end >= row * rowBits);
      const gap = el('div', 'row gap');
      gap.append(el('span', 'off'));
      gap.append(el('span', 'fold',
        `⋯ ${plural(skipped, 'row')} of ${over ? label(over.node, ms) || 'padding' : 'data'}`));
      map.append(gap);
    }
    prev = row;
    const lo = row * rowBits, hi = lo + rowBits;
    const pieces: Piece[] = all
      .filter((l) => l.start < hi && l.end > lo)
      .map((l) => ({ leaf: l, s: Math.max(l.start, lo) - lo, e: Math.min(l.end, hi) - lo }));

    // Overlapping members (unions) go on separate lanes.
    const laneEnds: number[] = [];
    const line = el('div', 'row');
    // Rows are at most 8 bytes and cache lines at least that, so each
    // boundary falls at the start of a row.
    if (cacheLine && lo && (lo / 8) % cacheLine === 0) {
      line.classList.add('cacheline');
      line.title = `cache line ${lo / 8 / cacheLine} (byte ${lo / 8})`;
    }
    line.append(el('span', 'off', String(lo / 8)));
    const lanes = el('div', 'lanes');
    for (const p of pieces) {
      let lane = laneEnds.findIndex((end) => end <= p.s);
      if (lane < 0) lane = laneEnds.push(0) - 1;
      laneEnds[lane] = p.e;
      const seg = el('div', `seg cat-${p.leaf.cat}`);
      seg.style.gridColumn = `${p.s + 1} / ${p.e + 1}`;
      seg.style.gridRow = String(lane + 1);
      if (p.leaf.start < lo) seg.classList.add('cont-l');
      if (p.leaf.end > hi) seg.classList.add('cont-r');
      if (p.e - p.s >= 8 && p.leaf.start >= lo) seg.textContent = label(p.leaf.node, ms);
      seg.dataset.path = p.leaf.path;
      seg.dataset.tip = tip(p.leaf.node, ms);
      lanes.append(seg);
    }
    line.append(lanes);
    map.append(line);
  }
  return map;
}

function legend(all: Leaf[]): HTMLElement {
  const present = new Set(all.map((l) => l.cat));
  const box = el('div', 'legend');
  for (const [cat, text] of CATEGORIES) {
    if (!present.has(cat)) continue;
    const item = el('span');
    item.append(el('span', `sw cat-${cat}`), text);
    box.append(item);
  }
  return box;
}

// --- Member table ---------------------------------------------------------

function members(
  r: LayoutRecord, cats: Map<string, Category>, ms: boolean, cacheLine: number,
  vtableLink: (n: LayoutNode) => HTMLElement | undefined,
): HTMLElement {
  const table = el('table', 'members');
  const head = el('tr');
  head.append(el('th', 'num', 'offset'), el('th', 'num', 'size'), el('th', 'num', 'align'),
              el('th', '', ''));
  table.append(el('thead'));
  table.tHead!.append(head);
  const body = el('tbody');
  // Rows come in tree order, which is offset order except for members that
  // overlap others, so mark each boundary before the first row that starts
  // at or after it.
  const line = cacheLine * 8;
  let next = line;
  const walk = (nodes: LayoutNode[], prefix: string, depth: number) => {
    nodes.forEach((n, i) => {
      const path = prefix ? `${prefix}.${i}` : `${i}`;
      const [start] = bits(n);
      if (line && start >= next) {
        while (start >= next + line) next += line;
        const mark = el('tr', 'cacheline');
        mark.append(el('td'), el('td'), el('td'));
        mark.append(el('td', '', `cache line ${next / line} (byte ${next / 8})`));
        body.append(mark);
        next += line;
      }
      const tr = el('tr');
      tr.dataset.path = path;
      tr.append(el('td', 'num', fmtOffset(start)), el('td', 'num', fmtSizeOf(n)),
                el('td', 'num', fmtAlign(n)));
      const td = el('td', 'desc');
      td.style.paddingLeft = `${depth * 1.25}em`;
      const cat = cats.get(path);
      td.append(el('span', cat ? `sw cat-${cat}` : 'sw'));
      describe(td, n, ms, vtableLink(n));
      notes(td, n);
      tr.append(td);
      body.append(tr);
      if (n.kind === 'record' && n.members) walk(n.members, path, depth + 1);
    });
  };
  walk(r.members, '', 0);
  if (!r.members.length) {
    const tr = el('tr');
    const td = el('td', 'dim', '(no members)');
    td.colSpan = 4;
    tr.append(td);
    body.append(tr);
  }
  table.append(body);
  return table;
}

// --- Vtables --------------------------------------------------------------

function thunkText(t: Thunk): string {
  let s = `thunk: this ${signed(t.this_adjustment)}`;
  if (t.vcall_offset_offset) s += `, vcall offset at ${t.vcall_offset_offset}`;
  else if (t.virtual_this_adjustment) s += ', virtual this adjustment';
  if (t.return_adjustment) s += `, return ${signed(t.return_adjustment)}`;
  if (t.vbase_offset_offset) s += `, vbase offset at ${t.vbase_offset_offset}`;
  else if (t.virtual_return_adjustment) s += ', virtual return adjustment';
  return s;
}

function entry(td: HTMLElement, e: Entry, ms: boolean) {
  const chip = (text: string, cls = '') => span(td, `chip ${cls}`, text);
  switch (e.kind) {
    case 'vcall_offset':
    case 'vbase_offset':
    case 'offset_to_top':
      span(td, 'dim', e.kind);
      td.append(` ${e.value}`);
      break;
    case 'rtti':
      span(td, 'dim', 'RTTI');
      td.append(` ${e.type}`);
      break;
    default:
      td.append(e.function ?? '');
      if (e.kind === 'complete_dtor') chip('complete');
      else if (e.kind === 'deleting_dtor') chip(ms ? 'scalar deleting' : 'deleting');
      else if (e.kind === 'unused_function') chip('unused');
      if (e.pure) chip('pure', 'pure');
  }
  if (e.thunk) chip(thunkText(e.thunk), 'thunk');
}

function vtable(vt: VTable, ms: boolean, id: string): HTMLElement {
  const box = el('div', 'vtable');
  const h = el('h3', '', vt.name);
  span(h, 'dim', `  ${vt.vfptr_offset !== undefined ? `vfptr at ${vt.vfptr_offset}, ` : ''}` +
                 plural(vt.entries.length, 'entry', 'entries'));
  box.append(h);
  const table = el('table', 'entries');
  const points = (index: number) => {
    for (const ap of vt.address_points) {
      if (ap.index !== index) continue;
      const tr = el('tr', 'point');
      const td = el('td', '', `address point: ${ap.subobjects.map((s) => `${s.type} @${s.offset}`).join(', ')}`);
      td.colSpan = 2;
      tr.append(el('td'), td);
      table.append(tr);
    }
  };
  points(0);
  vt.entries.forEach((e, i) => {
    const tr = el('tr');
    tr.id = `${id}-e${i}`;
    if (i && vt.table_starts?.includes(i)) tr.classList.add('start');
    tr.append(el('td', 'num', String(i)));
    const td = el('td');
    entry(td, e, ms);
    tr.append(td);
    table.append(tr);
    points(i + 1);
  });
  box.append(table);
  return box;
}

// --- Records --------------------------------------------------------------

function heading(r: LayoutRecord, hooks: Hooks): HTMLElement {
  const h = el('h2');
  span(h, 'tag', r.tag);
  const name = el('button', 'name', r.name);
  name.title = `Line ${r.location.line}`;
  name.onclick = () => hooks.locate(r.location.line, r.location.column);
  h.append(' ', name);
  return h;
}

/** "size 16, align 8 from d", with what set the alignment in code type. */
function sizeAlign(parent: HTMLElement, r: LayoutRecord) {
  parent.append(`size ${r.size}, align ${r.align}`);
  if (r.align_source?.text) {
    parent.append(' from ');
    span(parent, 'src', r.align_source.text);
  }
}

function summary(r: LayoutRecord, endian: Report['endian']): HTMLElement {
  const p = el('p', 'summary');
  sizeAlign(p, r);
  let rest = '';
  if (r.data_size !== r.size) rest += `, data size ${r.data_size}`;
  if (r.nv_size !== r.size && r.nv_size !== r.data_size) rest += `, non-virtual size ${r.nv_size}`;
  if (r.nv_align !== r.align) rest += `, non-virtual align ${r.nv_align}`;
  if (r.padding_bits) rest += `, padding ${fmtAmount(r.padding_bits)}`;
  if (hasBitFields(r.members)) rest += `, ${endian}-endian`;
  p.append(rest);
  return p;
}

function traits(r: LayoutRecord): HTMLElement | undefined {
  if (!r.traits) return undefined;
  const p = el('p', 'traits');
  const items: [string, boolean][] = [
    ['is_standard_layout', r.traits.standard_layout],
    ['is_trivially_copyable', r.traits.trivially_copyable],
    ['has_unique_object_representations', r.traits.unique_object_representations],
  ];
  for (const [name, yes] of items) {
    const item = el('span', yes ? 'yes' : 'no');
    item.append(el('code', '', name), ` ${yes ? 'yes' : 'no'}`);
    p.append(item);
  }
  return p;
}

function reorder(r: LayoutRecord): HTMLElement | undefined {
  if (!r.reorder) return undefined;
  const p = el('p', 'reorder');
  p.append('Reordering the fields ');
  span(p, 'saves', `saves ${fmtAmount((r.size - r.reorder.size) * 8)}`);
  p.append(` (size ${r.reorder.size}): `);
  r.reorder.fields.forEach((f, i) => {
    if (i) p.append(', ');
    p.append(el('code', '', f || '(unnamed)'));
  });
  return p;
}

/** Hovering a member highlights it in every view; hovering a base or a
 *  field of record type highlights everything inside it. An element can
 *  stand for several members (one per target), separated by spaces. */
function linkHover(card: HTMLElement) {
  const marked: Element[] = [];
  const clear = () => {
    for (const m of marked.splice(0)) m.classList.remove('hl');
  };
  card.addEventListener('pointerover', (ev) => {
    const target = (ev.target as Element).closest<HTMLElement>('[data-path]');
    clear();
    card.classList.toggle('hovering', !!target);
    if (!target) return;
    const ps = target.dataset.path!.split(' ');
    for (const m of card.querySelectorAll<HTMLElement>('[data-path]')) {
      const qs = m.dataset.path!.split(' ');
      if (qs.some((q) => ps.some((p) => q === p || q.startsWith(`${p}.`)))) {
        m.classList.add('hl');
        marked.push(m);
      }
    }
  });
  card.addEventListener('pointerleave', () => {
    clear();
    card.classList.remove('hovering');
  });
}

function record(r: LayoutRecord, ri: number, report: Report, hooks: Hooks): HTMLElement {
  const ms = report.abi === 'microsoft';
  const cacheLine = report.cache_line ?? 0;
  const card = el('article', 'card');
  card.append(heading(r, hooks), summary(r, report.endian));
  const t = traits(r);
  if (t) card.append(t);

  const all = leaves(r.members, '', false, []);
  if (r.size > 0 && all.length) card.append(legend(all), byteMap(r, all, ms, cacheLine));

  const ids = new Map(r.vtables.map((vt, vi) => [vt.name, `r${ri}-v${vi}`]));
  const vtableLink = (n: LayoutNode) => {
    const id = n.points_to && ids.get(n.points_to.table);
    if (!n.points_to || !id) return undefined;
    const a = el('a', 'link', `-> ${n.points_to.table}[${n.points_to.slot}]`);
    a.href = `#${id}-e${n.points_to.slot}`;
    a.onclick = (ev) => {
      ev.preventDefault();
      const row = document.getElementById(`${id}-e${n.points_to!.slot}`);
      row?.scrollIntoView({ block: 'center', behavior: 'smooth' });
      row?.classList.remove('flash');
      void row?.offsetWidth;
      row?.classList.add('flash');
    };
    return a;
  };
  card.append(members(r, new Map(all.map((l) => [l.path, l.cat])), ms, cacheLine, vtableLink));
  const re = reorder(r);
  if (re) card.append(re);
  r.vtables.forEach((vt, vi) => card.append(vtable(vt, ms, `r${ri}-v${vi}`)));
  linkHover(card);
  return card;
}

function abiText(report: Report): string {
  return `${report.abi === 'microsoft' ? 'Microsoft' : 'Itanium'} C++ ABI · ` +
    `${report.pointer_size}-byte pointers · ${report.endian}-endian`;
}

// --- Comparing two targets ------------------------------------------------

// One row of a comparison: the same member on each target, or on one only,
// with its path in each tree.
interface Pair {
  a?: LayoutNode;
  b?: LayoutNode;
  pa?: string;
  pb?: string;
  depth: number;
}

/** Identifies each member among its siblings across targets, as keys() in
 *  src/Text.cpp. Padding is keyed by the member it follows, so a hole that
 *  moves shows as two rows. */
function keys(nodes: LayoutNode[]): string[] {
  const seen = new Map<string, number>();
  let prev = 'start';
  return nodes.map((c) => {
    let k: string;
    if (c.kind === 'padding') {
      k = `padding after ${prev}`;
    } else {
      k = `${c.kind}|${c.role ?? ''}|${c.type ?? ''}|${c.name ?? ''}`;
      prev = k;
    }
    const n = seen.get(k) ?? 0;
    seen.set(k, n + 1);
    return `${k}#${n}`;
  });
}

function side(n: LayoutNode, path: string, depth: number, isA: boolean, out: Pair[]) {
  out.push(isA ? { a: n, pa: path, depth } : { b: n, pb: path, depth });
  if (n.kind === 'record')
    n.members?.forEach((c, i) => side(c, `${path}.${i}`, depth + 1, isA, out));
}

/** Interleaves the children of a and b, in a's order, with members found
 *  only in b placed after the member they follow there. */
function merge(a: LayoutNode[], b: LayoutNode[], pa: string, pb: string, depth: number,
               out: Pair[]) {
  const ka = keys(a), kb = keys(b);
  const inB = new Map(kb.map((k, j) => [k, j]));
  const inA = new Set(ka);
  const used = kb.map(() => false);
  let next = 0;
  const onlyB = (upTo: number) => {
    for (; next < upTo; ++next) {
      if (!used[next] && !inA.has(kb[next])) {
        used[next] = true;
        side(b[next], `${pb}.${next}`, depth, false, out);
      }
    }
  };
  ka.forEach((k, i) => {
    const x = a[i];
    const j = inB.get(k);
    if (j === undefined) {
      side(x, `${pa}.${i}`, depth, true, out);
      return;
    }
    onlyB(j);
    used[j] = true;
    const y = b[j];
    out.push({ a: x, b: y, pa: `${pa}.${i}`, pb: `${pb}.${j}`, depth });
    if (x.kind === 'record' && y.kind === 'record')
      merge(x.members ?? [], y.members ?? [], `${pa}.${i}`, `${pb}.${j}`, depth + 1, out);
    next = Math.max(next, j + 1);
  });
  onlyB(kb.length);
}

const cells = (n?: LayoutNode) =>
  (n ? [fmtOffset(bits(n)[0]), fmtSizeOf(n), fmtAlign(n)] : ['', '', '']);

function sameRow(p: Pair): boolean {
  if (!p.a || !p.b) return false;
  const [a0, a1] = bits(p.a), [b0, b1] = bits(p.b);
  return a0 === b0 && a1 - a0 === b1 - b0 && (p.a.align ?? 0) === (p.b.align ?? 0);
}

function compareTable(rows: Pair[], a: Report, b: Report, cats: Map<string, Category>): HTMLElement {
  const table = el('table', 'members compare');
  const head = el('thead');
  const top = el('tr', 'targets');
  top.append(el('th', 'mark'));
  for (const t of [a.target, b.target]) {
    const th = el('th', 'group', t);
    th.colSpan = 3;
    top.append(th);
  }
  top.append(el('th'));
  const cols = el('tr');
  cols.append(el('th', 'mark'));
  for (let k = 0; k < 2; k++) {
    for (const c of ['offset', 'size', 'align']) {
      cols.append(el('th', `num${k && c === 'offset' ? ' split' : ''}`, c));
    }
  }
  cols.append(el('th'));
  head.append(top, cols);
  table.append(head);

  const body = el('tbody');
  for (const p of rows) {
    const changed = !sameRow(p);
    const tr = el('tr', changed ? 'changed' : '');
    tr.dataset.path = [p.pa, p.pb].filter(Boolean).join(' ');
    const mark = tr.appendChild(el('td', 'mark', changed ? '*' : ''));
    if (changed) mark.title = p.a && p.b ? 'Differs between the targets'
                                         : `Only on ${p.a ? a.target : b.target}`;
    const ca = cells(p.a), cb = cells(p.b);
    for (const [k, cs] of [ca, cb].entries()) {
      cs.forEach((c, i) => {
        let cls = 'num';
        if (k && i === 0) cls += ' split';
        // A member on one target only is marked by the blank cells opposite.
        if (p.a && p.b && ca[i] !== cb[i]) cls += ' diff';
        tr.append(el('td', cls, c));
      });
    }
    const td = el('td', 'desc');
    td.style.paddingLeft = `${p.depth * 1.25}em`;
    const cat = cats.get(p.pa ?? '') ?? cats.get(p.pb ?? '');
    td.append(el('span', cat ? `sw cat-${cat}` : 'sw'));
    if (p.a) describe(td, p.a, a.abi === 'microsoft');
    else describe(td, p.b!, b.abi === 'microsoft');
    tr.append(td);
    body.append(tr);
  }
  table.append(body);
  return table;
}

function compareRecord(ra: LayoutRecord, rb: LayoutRecord, a: Report, b: Report,
                       hooks: Hooks): HTMLElement {
  const card = el('article', 'card');
  card.append(heading(ra, hooks));
  const rows: Pair[] = [];
  merge(ra.members, rb.members, 'a', 'b', 0, rows);
  if (ra.size === rb.size && ra.align === rb.align && rows.every(sameRow)) {
    card.append(el('p', 'summary', `same on both targets: size ${ra.size}, align ${ra.align}`));
    return card;
  }

  const sides = el('div', 'sides');
  for (const [r, report] of [[ra, a], [rb, b]] as const) {
    sides.append(el('span', 'target', report.target));
    const s = el('span', 'summary');
    sizeAlign(s, r);
    if (r.padding_bits) s.append(`, padding ${fmtAmount(r.padding_bits)}`);
    sides.append(s);
  }
  card.append(sides);

  const la = leaves(ra.members, 'a', false, []);
  const lb = leaves(rb.members, 'b', false, []);
  const all = [...la, ...lb];
  if (all.length) card.append(legend(all));
  for (const [r, report, ls] of [[ra, a, la], [rb, b, lb]] as const) {
    if (r.size > 0 && ls.length) {
      card.append(el('p', 'maplabel', report.target),
                  byteMap(r, ls, report.abi === 'microsoft'));
    }
  }
  card.append(compareTable(rows, a, b, new Map(all.map((l) => [l.path, l.cat]))));
  linkHover(card);
  return card;
}

function onlyOn(r: LayoutRecord, target: string, hooks: Hooks): HTMLElement {
  const card = el('article', 'card');
  card.append(heading(r, hooks), el('p', 'summary', `only on ${target}`));
  return card;
}

function renderCompare(frag: DocumentFragment, a: Report, b: Report, hooks: Hooks) {
  frag.append(el('p', 'meta', `${a.target} (${abiText(a)}) compared with ` +
                              `${b.target} (${abiText(b)})`));
  const inB = new Map(b.records.map((r) => [r.name, r]));
  const inA = new Set(a.records.map((r) => r.name));
  for (const r of a.records) {
    const other = inB.get(r.name);
    frag.append(other ? compareRecord(r, other, a, b, hooks) : onlyOn(r, a.target, hooks));
  }
  for (const r of b.records) if (!inA.has(r.name)) frag.append(onlyOn(r, b.target, hooks));
  if (!a.records.length && !b.records.length)
    frag.append(el('p', 'empty', 'No records. Declare a struct, class or union.'));
}

/** Draws report, or with compare, the differences between the two. */
export function render(out: HTMLElement, report: Report, hooks: Hooks, compare?: Report) {
  const frag = document.createDocumentFragment();
  if (compare) {
    renderCompare(frag, report, compare, hooks);
    out.append(frag);
    return;
  }
  let meta = `${report.target} · ${abiText(report)}`;
  if (report.cache_line) meta += ` · ${report.cache_line}-byte cache lines`;
  frag.append(el('p', 'meta', `${meta} · ${plural(report.records.length, 'record')}`));
  if (!report.records.length)
    frag.append(el('p', 'empty', 'No records. Declare a struct, class or union.'));
  report.records.forEach((r, i) => frag.append(record(r, i, report, hooks)));
  out.append(frag);
}

/** Shows the data-tip of whatever the pointer is over inside root. */
export function tooltips(root: HTMLElement, box: HTMLElement) {
  root.addEventListener('pointermove', (ev) => {
    const t = (ev.target as Element).closest<HTMLElement>('[data-tip]');
    if (!t) {
      box.hidden = true;
      return;
    }
    box.textContent = t.dataset.tip!;
    box.hidden = false;
    const w = box.offsetWidth, h = box.offsetHeight;
    const x = ev.clientX + 14 + w > innerWidth ? ev.clientX - 14 - w : ev.clientX + 14;
    const y = ev.clientY + 14 + h > innerHeight ? ev.clientY - 14 - h : ev.clientY + 14;
    box.style.left = `${Math.max(4, x)}px`;
    box.style.top = `${Math.max(4, y)}px`;
  });
  root.addEventListener('pointerleave', () => (box.hidden = true));
}
