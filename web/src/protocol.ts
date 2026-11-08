// Messages between the page and the worker that runs lense.
import type { AnalyzeResult, Options } from './types';

export type ToWorker =
  | { type: 'init'; url: string }
  | { type: 'analyze'; id: number; code: string; options: Options };

export type FromWorker =
  | { type: 'status'; text: string }
  | { type: 'ready'; ms: number }
  | { type: 'fatal'; message: string }
  | { type: 'result'; id: number; result: AnalyzeResult; stderr: string; ms: number };
