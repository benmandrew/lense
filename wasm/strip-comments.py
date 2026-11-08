#!/usr/bin/env python3
"""Removes C and C++ comments from every header under a directory, in place.

Newlines are kept, so line numbers in clang's diagnostics stay right. String,
character and raw string literals are copied unchanged. Comments are about a
quarter of the staged headers (mostly documentation in the intrinsics
headers), and clang does not need them to compute a layout.

    strip-comments.py <dir>
"""
import os
import re
import sys

RAW = re.compile(r'"([^()\\\s]{0,16})\(')


def strip(src: str) -> str:
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ''
        if c == '/' and nxt == '/':
            j = i + 2
            while j < n and src[j] != '\n':
                if src[j] == '\\' and src[j + 1:j + 2] == '\n':  # continued comment
                    out.append('\n')
                    j += 2
                else:
                    j += 1
            i = j
        elif c == '/' and nxt == '*':
            end = src.find('*/', i + 2)
            end = n if end < 0 else end + 2
            out.append(' ' + '\n' * src.count('\n', i, end))
            i = end
        elif c == '"' and i and src[i - 1] == 'R' and (m := RAW.match(src, i)):
            end = src.find(')' + m.group(1) + '"', i)
            end = n if end < 0 else end + len(m.group(1)) + 2
            out.append(src[i:end])
            i = end
        elif c in '"\'':
            j = i + 1
            while j < n and src[j] not in (c, '\n'):
                j += 2 if src[j] == '\\' else 1
            out.append(src[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return '\n'.join(line.rstrip() for line in ''.join(out).split('\n'))


def main(root: str) -> None:
    for d, _, files in os.walk(root):
        for f in files:
            path = os.path.join(d, f)
            if f.endswith('.modulemap'):
                continue
            with open(path, encoding='utf-8', errors='surrogateescape') as fh:
                src = fh.read()
            text = strip(src)
            if text.count('\n') != src.count('\n'):
                sys.exit(f'strip-comments.py: line count changed in {path}')
            with open(path, 'w', encoding='utf-8', errors='surrogateescape') as fh:
                fh.write(text)


if __name__ == '__main__':
    main(sys.argv[1])
