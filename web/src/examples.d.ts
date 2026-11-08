declare module 'virtual:examples' {
  /** The inputs in tests/cases, sorted by file name. */
  const examples: { name: string; lang: 'c' | 'c++'; code: string }[];
  export default examples;
}
