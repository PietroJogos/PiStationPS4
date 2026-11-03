# SPDX-License-Identifier: GPL-2.0-or-later
"""Index literal source registrations; never evaluate C++ or claim runtime coverage."""
import argparse
import hashlib
import json
from pathlib import Path
import re


TOKENS = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
REGISTRATION = re.compile(
    r'\bLIB_(FUNCTION|OBJ)\s*\(\s*"([^"\\]*)"\s*,\s*"([^"\\]*)"\s*,\s*'
    r'(\d+)\s*,\s*"([^"\\]*)"\s*,\s*([\w:]+)\s*\)')


def masked(text, strings=False):
    def replace(match):
        token = match.group()
        if strings or token.startswith('//') or token.startswith('/*'):
            return ''.join('\n' if char == '\n' else ' ' for char in token)
        return token
    return TOKENS.sub(replace, text)


def stub_markers(text):
    code = masked(text, strings=True)
    result = set()
    for match in re.finditer(r'\bPS4_SYSV_ABI\s+(\w+)\s*\(', code):
        position, depth = match.end(), 1
        while position < len(code) and depth:
            depth += (code[position] == '(') - (code[position] == ')')
            position += 1
        while position < len(code) and code[position].isspace():
            position += 1
        if position == len(code) or code[position] != '{':
            continue
        start, depth = position, 1
        position += 1
        while position < len(code) and depth:
            depth += (code[position] == '{') - (code[position] == '}')
            position += 1
        if depth == 0 and '(STUBBED)' in masked(text[start:position]):
            result.add(match.group(1))
    return result


def registrations(text, relative):
    uncommented = masked(text)
    code = masked(text, strings=True)
    markers = stub_markers(text)
    records = []
    for match in REGISTRATION.finditer(uncommented):
        if code[match.start():match.start() + 4] != 'LIB_':
            continue
        kind, nid, library, version, module, function = match.groups()
        if not 0 <= int(version) <= 65535:
            continue
        source = f'{relative}:{text.count(chr(10), 0, match.start()) + 1}'
        evidence = ('source_contains_stub_marker' if function in markers else
                    'source_registration_behavior_unverified')
        records.append((nid, library, int(version), module, function,
                        2 if kind == 'FUNCTION' else 1, source, evidence))
    total = len(re.findall(r'\bLIB_(?:FUNCTION|OBJ)\s*\(', code))
    return records, total - len(records)


def generate(root, paths):
    records, skipped = [], 0
    digest = hashlib.sha256()
    for path in sorted(paths):
        relative = path.relative_to(root).as_posix()
        raw = path.read_bytes()
        digest.update(relative.encode() + b'\0' + raw + b'\0')
        found, omitted = registrations(raw.decode('utf-8'), relative)
        records.extend(found)
        skipped += omitted
    lines = ['// SPDX-License-Identifier: GPL-2.0-or-later',
             '// Generated literal source evidence, not a live HLE registry.',
             '#include "source_service_catalog.h"', 'namespace PiStation {',
             'std::span<const SourceServiceRecord> SourceServiceCatalog() {',
             'static constexpr SourceServiceRecord records[] = {']
    for record in records:
        lines.append('{' + ','.join(str(value) if isinstance(value, int) else
                                   json.dumps(value, ensure_ascii=True) for value in record) + '},')
    lines += ['}; return records; }',
              f'std::string_view SourceServiceCatalogDigest() {{ return "{digest.hexdigest()}"; }}',
              f'std::size_t SourceServiceCatalogSkipped() {{ return {skipped}; }}', '}']
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('sources', type=Path, nargs='+')
    args = parser.parse_args()
    args.output.write_text(generate(args.root.resolve(), [p.resolve() for p in args.sources]),
                           encoding='utf-8')
