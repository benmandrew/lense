// The JSON that lense prints (src/Json.cpp) and that lense_analyze returns.

export interface Span {
  offset: number;
  size?: number;
  bit_offset?: number;
  bit_size?: number;
}

export type NodeKind =
  | 'record' | 'field' | 'bitfield' | 'vptr' | 'vbptr' | 'vtordisp' | 'padding';

export interface LayoutNode extends Span {
  kind: NodeKind;
  role?: 'field' | 'primary_base' | 'base' | 'primary_virtual_base' | 'virtual_base';
  tag?: string;
  type?: string;
  name?: string;
  empty?: boolean;
  /** Alignment in bytes; absent on bitfields and padding. */
  align?: number;
  /** Placed below its natural alignment (packed, #pragma pack). */
  unaligned?: true;
  crosses_cache_line?: true;
  /** Bitfields only: which memory location (C++ [intro.memory]) it is in. */
  memory_location?: number;
  points_to?: { table: string; slot: number };
  members?: LayoutNode[];
}

export interface Thunk {
  this_adjustment: number;
  vcall_offset_offset?: number;
  virtual_this_adjustment?: boolean;
  return_adjustment?: number;
  vbase_offset_offset?: number;
  virtual_return_adjustment?: boolean;
}

export interface Entry {
  index: number;
  kind:
    | 'vcall_offset' | 'vbase_offset' | 'offset_to_top' | 'rtti' | 'function'
    | 'complete_dtor' | 'deleting_dtor' | 'unused_function';
  value?: number;
  type?: string;
  function?: string;
  pure?: boolean;
  thunk?: Thunk;
}

export interface VTable {
  name: string;
  vfptr_offset?: number;
  table_starts?: number[];
  address_points: { index: number; subobjects: { type: string; offset: number }[] }[];
  entries: Entry[];
}

export interface AlignSource {
  kind: 'none' | 'member' | 'attribute' | 'packed' | 'pack';
  text?: string;
}

export interface Traits {
  standard_layout: boolean;
  trivially_copyable: boolean;
  unique_object_representations: boolean;
}

export interface Reorder {
  size: number;
  order: number[];
  /** Field names in the new order; "" is an unnamed field. */
  fields: string[];
}

export interface LayoutRecord {
  name: string;
  tag: string;
  location: { line: number; column: number };
  size: number;
  align: number;
  data_size: number;
  nv_size: number;
  nv_align: number;
  padding_bits: number;
  align_source: AlignSource;
  /** C++ only. */
  traits?: Traits;
  /** A field order that clang laid out smaller. */
  reorder?: Reorder;
  holes: Span[];
  members: LayoutNode[];
  vtables: VTable[];
}

export interface Report {
  target: string;
  abi: 'itanium' | 'microsoft';
  pointer_size: number;
  endian: 'little' | 'big';
  /** Bytes; absent when cache-line checks are off. */
  cache_line?: number;
  records: LayoutRecord[];
}

/** With options.compare, `compare` is the layout for that target. */
export type AnalyzeResult = { ok: boolean; report: Report; compare?: Report } | { error: string };

export interface Options {
  lang: 'c' | 'c++';
  target: string;
  vtables: boolean;
  expandSystem: boolean;
  args: string[];
  /** Bytes, a power of two; 0 turns cache-line checks off. */
  cacheLine: number;
  /** A second target triple to compare with; vtables are off then. */
  compare?: string;
}

/** [start, end) of a node or hole, in bits from the start of the record. */
export function bits(n: Span): [number, number] {
  if (n.bit_offset !== undefined)
    return [n.bit_offset, n.bit_offset + (n.bit_size ?? 0)];
  return [n.offset * 8, (n.offset + (n.size ?? 0)) * 8];
}
