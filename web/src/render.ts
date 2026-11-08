// Draws a lense report: per record, a byte map, the member table (worded
// like the CLI's text output) and the vtables.
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

function tip(n: LayoutNode, ms: boolean): string {
  const s = el('span');
  describe(s, n, ms);
  const [start, end] = bits(n);
  const inBits = n.kind === 'bitfield' || start % 8 !== 0 || end % 8 !== 0;
  const where = inBits
    ? `bits ${start}–${end - 1} (byte ${fmtOffset(start)}), ${plural(end - start, 'bit')}`
    : `offset ${start / 8}, ${plural((end - start) / 8, 'byte')}`;
  return `${s.textContent?.replace(/\s+/g, ' ').trim()}\n${where}`;
}

// --- Byte map -------------------------------------------------------------

interface Piece { leaf: Leaf; s: number; e: number }

function byteMap(r: LayoutRecord, all: Leaf[], ms: boolean): HTMLElement {
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
  r: LayoutRecord, cats: Map<string, Category>, ms: boolean,
  vtableLink: (n: LayoutNode) => HTMLElement | undefined,
): HTMLElement {
  const table = el('table', 'members');
  const head = el('tr');
  head.append(el('th', 'num', 'offset'), el('th', 'num', 'size'), el('th', '', ''));
  table.append(el('thead'));
  table.tHead!.append(head);
  const body = el('tbody');
  const walk = (nodes: LayoutNode[], prefix: string, depth: number) => {
    nodes.forEach((n, i) => {
      const path = prefix ? `${prefix}.${i}` : `${i}`;
      const [start, end] = bits(n);
      const inBits = n.kind === 'bitfield' ||
        (n.kind === 'padding' && (start % 8 !== 0 || end % 8 !== 0));
      const tr = el('tr');
      tr.dataset.path = path;
      tr.append(el('td', 'num', fmtOffset(start)), el('td', 'num', fmtSize(end - start, inBits)));
      const td = el('td', 'desc');
      td.style.paddingLeft = `${depth * 1.25}em`;
      const cat = cats.get(path);
      td.append(el('span', cat ? `sw cat-${cat}` : 'sw'));
      describe(td, n, ms, vtableLink(n));
      tr.append(td);
      body.append(tr);
      if (n.kind === 'record' && n.members) walk(n.members, path, depth + 1);
    });
  };
  walk(r.members, '', 0);
  if (!r.members.length) {
    const tr = el('tr');
    const td = el('td', 'dim', '(no members)');
    td.colSpan = 3;
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

function record(r: LayoutRecord, ri: number, ms: boolean, hooks: Hooks): HTMLElement {
  const card = el('article', 'card');
  const h = el('h2');
  span(h, 'tag', r.tag);
  const name = el('button', 'name', r.name);
  name.title = `Line ${r.location.line}`;
  name.onclick = () => hooks.locate(r.location.line, r.location.column);
  h.append(' ', name);
  card.append(h);
  let summary = `size ${r.size}, align ${r.align}`;
  if (r.data_size !== r.size) summary += `, data size ${r.data_size}`;
  if (r.padding_bits) summary += `, padding ${fmtAmount(r.padding_bits)}`;
  card.append(el('p', 'summary', summary));

  const all = leaves(r.members, '', false, []);
  if (r.size > 0 && all.length) card.append(legend(all), byteMap(r, all, ms));

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
  card.append(members(r, new Map(all.map((l) => [l.path, l.cat])), ms, vtableLink));
  r.vtables.forEach((vt, vi) => card.append(vtable(vt, ms, `r${ri}-v${vi}`)));

  // Hovering a member highlights it in both views; hovering a base or a
  // field of record type highlights everything inside it.
  const marked: Element[] = [];
  card.addEventListener('pointerover', (ev) => {
    const target = (ev.target as Element).closest<HTMLElement>('[data-path]');
    for (const m of marked.splice(0)) m.classList.remove('hl');
    card.classList.toggle('hovering', !!target);
    if (!target) return;
    const p = target.dataset.path!;
    for (const m of card.querySelectorAll<HTMLElement>('[data-path]')) {
      const q = m.dataset.path!;
      if (q === p || q.startsWith(`${p}.`)) {
        m.classList.add('hl');
        marked.push(m);
      }
    }
  });
  card.addEventListener('pointerleave', () => {
    for (const m of marked.splice(0)) m.classList.remove('hl');
    card.classList.remove('hovering');
  });
  return card;
}

export function render(out: HTMLElement, report: Report, hooks: Hooks) {
  const ms = report.abi === 'microsoft';
  const frag = document.createDocumentFragment();
  frag.append(el('p', 'meta',
    `${report.target} · ${ms ? 'Microsoft' : 'Itanium'} C++ ABI · ` +
    `${report.pointer_size}-byte pointers · ${plural(report.records.length, 'record')}`));
  if (!report.records.length)
    frag.append(el('p', 'empty', 'No records. Declare a struct, class or union.'));
  report.records.forEach((r, i) => frag.append(record(r, i, ms, hooks)));
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
