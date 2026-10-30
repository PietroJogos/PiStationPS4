# SPDX-License-Identifier: GPL-2.0-or-later
import argparse
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import unittest


parser = argparse.ArgumentParser()
parser.add_argument('--scanner', required=True, type=Path)
arguments, remaining = parser.parse_known_args()
SCANNER = arguments.scanner.resolve()


def elf_header(phnum=0, phoff=64, machine=62, elf_type=0xFE10):
    ident = b'\x7fELF\x02\x01\x01' + bytes(9)
    return struct.pack('<16sHHIQQQIHHHHHH', ident, elf_type, machine, 1, 0x1000,
                       phoff, 0, 0, 64, 56, phnum, 0, 0, 0)


def dynamic_elf(sce=False, name=b'libExample.sprx', name_offset=1, terminate=True):
    strings = b'\0' + name + b'\0'
    string_offset = 256
    dynamic_offset = 320
    address = 0x400000
    if sce:
        entries = [(0x61000035, 0), (0x61000037, len(strings)),
                   (0x6100000F, (7 << 48) | name_offset),
                   (0x61000015, (4 << 48) | name_offset), (1, name_offset)]
        data_type, data_address = 0x61000000, 0
    else:
        entries = [(1, name_offset), (5, address), (10, len(strings))]
        data_type, data_address = 1, address
    if terminate:
        entries.append((0, 0))
    dynamic = b''.join(struct.pack('<QQ', *entry) for entry in entries)
    data = bytearray(dynamic_offset + len(dynamic))
    data[:64] = elf_header(2)
    data[64:120] = struct.pack('<IIQQQQQQ', data_type, 4, string_offset,
                               data_address, 0, len(strings), len(strings), 1)
    data[120:176] = struct.pack('<IIQQQQQQ', 2, 4, dynamic_offset, 0, 0,
                                len(dynamic), len(dynamic), 8)
    data[string_offset:string_offset + len(strings)] = strings
    data[dynamic_offset:] = dynamic
    return data


def self_container(flags=None):
    count = 0 if flags is None else 1
    header_size = 32 + 32 * count
    size = header_size + (4 if count else 0)
    header = struct.pack('<I6BHHHQHHI', 0x1D3D154F, 0, 1, 1, 0, 0, 0,
                         0, header_size, 0, size, count, 0x22, 0)
    if count:
        return header + struct.pack('<QQQQ', flags, header_size, 4, 4) + b'TEST'
    return header


def analyzed_elf(needed=(), symbols=(), soname=None, relocations=(), tls=False,
                 sce=False, module='libExampleModule', exported_library='libExample',
                 imported_module=None, imported_library=None, library_version=1):
    base = 0x400000
    strings = bytearray(b'\0')

    def name_offset(name):
        offset = len(strings)
        strings.extend(name.encode('utf-8') + b'\0')
        return offset

    entries = [(1, name_offset(name)) for name in needed]
    if soname:
        entries.append((14, name_offset(soname)))
    if sce:
        entries.append((0x6100000D, name_offset(module) | (1 << 40) | (7 << 48)))
        if exported_library:
            entries.append((0x61000013, name_offset(exported_library) |
                            (library_version << 32) | (5 << 48)))
        if imported_module:
            entries.append((0x6100000F, name_offset(imported_module) | (1 << 40) | (3 << 48)))
        if imported_library:
            entries.append((0x61000015, name_offset(imported_library) | (1 << 32) | (2 << 48)))
    symbol_bytes = bytearray(24)
    for name, imported, symbol_type, weak in symbols:
        symbol_bytes.extend(struct.pack('<IBBHQQ', name_offset(name),
                                        ((2 if weak else 1) << 4) | symbol_type,
                                        0, 0 if imported else 1,
                                        0 if imported else base + 0x180, 8))
    data = bytearray(0x1800)
    count = 3 if sce or tls else 2
    data[:64] = elf_header(phnum=count, elf_type=3)
    struct.pack_into('<Q', data, 24, base + 0x180)
    if sce:
        entries += [(0x61000035, 0), (0x61000037, len(strings)),
                    (0x61000039, 0x200), (0x6100003F, len(symbol_bytes)), (0x6100003B, 24)]
    else:
        entries += [(5, base + 0x1000), (10, len(strings)), (6, base + 0x1200),
                    (11, 24), (4, base + 0x1400)]
    rela = b''.join(struct.pack('<QQq', address, (symbol << 32) | kind, addend)
                    for address, kind, symbol, addend in relocations)
    if rela:
        entries += [(7, base + 0x1500), (8, len(rela)), (9, 24)]
    entries.append((0, 0))
    dynamic = b''.join(struct.pack('<QQ', *entry) for entry in entries)
    assert len(dynamic) < 0xE00 and len(strings) < 0x200
    data[64:120] = struct.pack('<IIQQQQQQ', 1, 5, 0, base, 0, len(data), len(data), 0x1000)
    data[120:176] = struct.pack('<IIQQQQQQ', 2, 4, 0x200, base + 0x200, 0,
                                len(dynamic), len(dynamic), 8)
    if sce:
        data[176:232] = struct.pack('<IIQQQQQQ', 0x61000000, 4, 0x1000, 0, 0, 0x800, 0x800, 1)
    elif tls:
        data[176:232] = struct.pack('<IIQQQQQQ', 7, 4, 0x1700, base + 0x1700, 0, 4, 16, 4)
    data[0x200:0x200 + len(dynamic)] = dynamic
    data[0x1000:0x1000 + len(strings)] = strings
    data[0x1200:0x1200 + len(symbol_bytes)] = symbol_bytes
    chain_count = len(symbol_bytes) // 24
    hash_data = struct.pack('<II', 1, chain_count) + bytes(4 * (1 + chain_count))
    data[0x1400:0x1400 + len(hash_data)] = hash_data
    data[0x1500:0x1500 + len(rela)] = rela
    return data


class ScannerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pistation-synthetic-')
        self.base = Path(self.temp.name)
        self.root = self.base / 'input'
        self.root.mkdir()
        self.output = self.base / 'report.json'

    def tearDown(self):
        self.temp.cleanup()

    def write(self, path, data):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return target

    def scan(self, expected=0, root=None, output=None, extra=()):
        result = subprocess.run([str(SCANNER), '--firmware-dir', str(root or self.root),
                                 '--report', str(output or self.output), *extra],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        report_path = output or self.output
        report = json.loads(report_path.read_text(encoding='utf-8')) if report_path.exists() else None
        return result, report

    def test_help(self):
        result = subprocess.run([str(SCANNER), '--help'], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0)
        self.assertIn('offline diagnostics', result.stdout)

    def test_missing_directory_is_reported(self):
        result, report = self.scan(1, root=self.base / 'missing')
        self.assertFalse(report['input_valid'])
        self.assertFalse(report['scan_complete'])
        self.assertIn('[ERROR]', result.stdout)

    def test_file_is_not_directory(self):
        file = self.write('plain', b'data')
        _, report = self.scan(1, root=file)
        self.assertFalse(report['input_valid'])

    def test_empty_directory(self):
        _, report = self.scan(1)
        self.assertEqual(report['files'], [])
        self.assertGreater(report['error_count'], 0)

    def test_identification_requires_magic(self):
        self.write('SceShellUI.self', b'plain text')
        self.write('sceShellCore.sprx', elf_header())
        _, report = self.scan()
        files = {f['path']: f for f in report['files']}
        self.assertEqual(files['SceShellUI.self']['kind'], 'unknown')
        self.assertEqual(files['SceShellUI.self']['status'], 'unrecognized_executable_extension')
        self.assertEqual(files['SceShellUI.self']['shell_role'], 'SceShellUI_name_candidate')
        self.assertEqual(files['sceShellCore.sprx']['shell_role'], 'SceShellCore_name_candidate')

    def test_recursive_magic_and_order(self):
        self.write('z/unusual.name', elf_header())
        self.write('a', b'')
        _, report = self.scan()
        self.assertEqual([f['path'] for f in report['files']], ['a', 'z/unusual.name'])
        metadata = report['files'][1]
        self.assertEqual(metadata['kind'], 'ELF')
        self.assertEqual(metadata['elf']['entry'], '0x1000')
        self.assertEqual(metadata['elf']['machine'], 62)
        self.assertFalse(report['boot_attempted'])
        self.assertFalse(report['firmware_version_verified'])

    def test_truncated_executables_continue_scanning(self):
        self.write('bad.elf', b'\x7fELF')
        self.write('bad.self', b'\x4f\x15\x3d\x1d')
        self.write('good.elf', elf_header())
        _, report = self.scan(1)
        self.assertEqual(report['error_count'], 2)
        self.assertEqual(len(report['files']), 3)
        self.assertEqual(report['files'][2]['status'], 'metadata_validated')

    def test_unsupported_machine(self):
        self.write('arm.elf', elf_header(machine=183))
        _, report = self.scan()
        self.assertEqual(report['files'][0]['status'], 'unsupported')

    def test_unsupported_elf_encoding(self):
        data = bytearray(elf_header())
        data[5] = 2
        self.write('big.elf', data)
        _, report = self.scan()
        self.assertEqual(report['files'][0]['status'], 'unsupported')

    def test_elf32_is_recognized_without_reading_elf64_header(self):
        data = bytearray(52)
        data[:7] = b'\x7fELF\x01\x01\x01'
        self.write('elf32', data)
        _, report = self.scan()
        self.assertEqual(report['files'][0]['kind'], 'ELF')
        self.assertEqual(report['files'][0]['status'], 'unsupported')

    def test_bad_header_version(self):
        data = bytearray(elf_header())
        data[6] = 0
        self.write('bad', data)
        _, report = self.scan(1)
        self.assertEqual(report['files'][0]['status'], 'invalid_or_unreadable')

    def test_header_table_overflow(self):
        self.write('overflow', elf_header(phnum=1, phoff=2**64-32))
        _, report = self.scan(1)
        self.assertIn('range', report['files'][0]['notes'][0])

    def test_elf_program_header_limit(self):
        self.write('excess', elf_header(phnum=65535))
        self.scan(1)

    def test_standard_declared_dependency_and_presence_only(self):
        self.write('SceShellUI.elf', dynamic_elf())
        self.write('system/libExample.sprx', b'not a valid module')
        result, report = self.scan()
        dependency = report['files'][0]['dependencies'][0]
        self.assertEqual(dependency['name'], 'libExample.sprx')
        self.assertEqual(dependency['filename_candidates'], ['system/libExample.sprx'])
        self.assertEqual(dependency['resolution'], 'not_evaluated')
        for category in ['PISTATION', 'FIRMWARE900', 'LOADER', 'MODULES', 'DEPENDENCIES']:
            self.assertIn(f'[{category}]', result.stdout)

    def test_sce_declared_modules_and_libraries(self):
        self.write('shell.elf', dynamic_elf(sce=True, name=b'libSceExample'))
        self.write('libSceExample.sprx', elf_header())
        _, report = self.scan()
        dependencies = report['files'][1]['dependencies']
        self.assertEqual([d['kind'] for d in dependencies],
                         ['DT_SCE_NEEDED_MODULE', 'DT_SCE_IMPORT_LIB', 'DT_NEEDED'])
        self.assertTrue(all(d['filename_candidates'] == ['libSceExample.sprx'] for d in dependencies))

    def test_absent_name_does_not_claim_unresolved_symbol(self):
        self.write('shell', dynamic_elf())
        _, report = self.scan()
        self.assertEqual(report['files'][0]['dependencies'][0]['filename_candidates'], [])
        self.assertEqual(report['symbol_resolution'], 'not_evaluated')

    def test_bad_dependency_offset(self):
        self.write('bad', dynamic_elf(name_offset=2**32))
        _, report = self.scan(1)
        self.assertEqual(report['files'][0]['dependencies'], [])

    def test_dependency_name_limit(self):
        string_data = b'\0' + b'x' * 1025 + b'\0'
        dynamic = b''.join(struct.pack('<QQ', *pair) for pair in
                           [(1, 1), (5, 0x400000), (10, len(string_data)), (0, 0)])
        data = bytearray(256 + len(string_data) + len(dynamic))
        data[:64] = elf_header(2)
        data[64:120] = struct.pack('<IIQQQQQQ', 1, 4, 256, 0x400000, 0,
                                   len(string_data), len(string_data), 1)
        data[120:176] = struct.pack('<IIQQQQQQ', 2, 4, 256 + len(string_data), 0, 0,
                                    len(dynamic), len(dynamic), 8)
        data[256:256 + len(string_data)] = string_data
        data[256 + len(string_data):] = dynamic
        self.write('bad', data)
        self.scan(1)

    def test_dependency_table_count_limit(self):
        data = dynamic_elf()
        dynamic = struct.pack('<QQ', 1, 1) * 4097 + struct.pack('<QQ', 0, 0)
        data = data[:320] + dynamic
        struct.pack_into('<Q', data, 120 + 32, len(dynamic))
        self.write('excess', data)
        _, report = self.scan(1)
        self.assertEqual(report['files'][0]['dependencies'], [])

    def test_total_dependency_limit_marks_incomplete(self):
        data = dynamic_elf()
        entries = [(5, 0x400000), (10, len(b'\0libExample.sprx\0'))]
        entries += [(1, 1)] * 4096 + [(0, 0)]
        dynamic = b''.join(struct.pack('<QQ', *entry) for entry in entries)
        data = data[:320] + dynamic
        struct.pack_into('<Q', data, 120 + 32, len(dynamic))
        for i in range(3):
            self.write(f'{i}.elf', data)
        _, report = self.scan(1)
        self.assertFalse(report['scan_complete'])
        self.assertEqual(sum(len(f['dependencies']) for f in report['files']), 8192)

    def test_candidate_limit_is_reported(self):
        self.write('shell', dynamic_elf())
        for i in range(33):
            self.write(f'{i}/libExample.sprx', b'placeholder')
        _, report = self.scan(1)
        self.assertFalse(report['scan_complete'])
        file = next(f for f in report['files'] if f['path'] == 'shell')
        dependency = file['dependencies'][0]
        self.assertTrue(dependency['filename_candidates_truncated'])
        self.assertEqual(len(dependency['filename_candidates']), 32)

    def test_unterminated_dynamic_table(self):
        self.write('bad', dynamic_elf(terminate=False))
        self.scan(1)

    def test_unterminated_dependency_name(self):
        data = dynamic_elf()
        data[256 + len(b'\0libExample.sprx')] = ord('X')
        self.write('bad', data)
        self.scan(1)

    def test_encrypted_self_is_header_only(self):
        data = self_container(flags=2 | 8)
        target = self.write('SceShellCore.self', data)
        _, report = self.scan()
        file = report['files'][0]
        self.assertEqual(file['kind'], 'SELF')
        self.assertEqual(file['status'], 'container_header_only')
        self.assertTrue(file['self']['encrypted_segments'])
        self.assertTrue(file['self']['compressed_segments'])
        self.assertEqual(file['dependency_status'], 'self_payload_not_examined')
        self.assertEqual(target.read_bytes(), data)

    def test_invalid_self_segment(self):
        data = bytearray(self_container(flags=0))
        struct.pack_into('<Q', data, 40, 2**64-1)
        self.write('bad.self', data)
        self.scan(1)

    def test_archive_and_psf_are_not_extracted(self):
        self.write('9.00.zip', b'PK\x03\x04synthetic')
        self.write('param.sfo', b'\0PSFsynthetic')
        _, report = self.scan()
        self.assertEqual([f['kind'] for f in report['files']], ['ZIP', 'PSF'])
        self.assertEqual(report['files'][0]['status'], 'archive_not_extracted')
        self.assertEqual(len(list(self.root.iterdir())), 2)

    @unittest.skipUnless(os.name == 'posix', 'POSIX symlink test')
    def test_symlinks_are_not_followed(self):
        external = self.base / 'outside'
        external.mkdir()
        (external / 'secret.elf').write_bytes(elf_header())
        self.write('inside.elf', elf_header())
        (self.root / 'external').symlink_to(external, target_is_directory=True)
        (self.root / 'linked').symlink_to(external / 'secret.elf')
        (self.root / 'loop').symlink_to(self.root, target_is_directory=True)
        _, report = self.scan()
        self.assertEqual([f['path'] for f in report['files']], ['inside.elf'])

    @unittest.skipUnless(os.name == 'posix', 'POSIX FIFO test')
    def test_fifo_is_not_opened(self):
        os.mkfifo(self.root / 'pipe')
        self.write('file', b'normal')
        _, report = self.scan()
        self.assertEqual(len(report['files']), 1)

    def test_limit_marks_partial_scan(self):
        self.write('a', b'a')
        self.write('b', b'b')
        _, report = self.scan(1, extra=('--max-entries', '1'))
        self.assertFalse(report['scan_complete'])
        self.assertEqual(len(report['files']), 1)

    def test_json_and_logs_escape_filename(self):
        self.write('test"\n[ERROR]\\é.elf', elf_header())
        result, report = self.scan()
        self.assertEqual(report['files'][0]['path'], 'test"\n[ERROR]\\é.elf')
        self.assertNotIn('\n[ERROR]', result.stdout)

    @unittest.skipUnless(os.name == 'posix', 'POSIX invalid UTF-8 filename test')
    def test_invalid_utf8_has_valid_json(self):
        path = os.fsencode(self.root) + b'/invalid-\xff.elf'
        with open(path, 'wb') as stream:
            stream.write(elf_header())
        _, report = self.scan()
        self.assertIn('\ufffd', report['files'][0]['path'])

    def test_report_inside_input_is_refused(self):
        self.write('file', b'file')
        result = subprocess.run([str(SCANNER), '--firmware-dir', str(self.root),
                                 '--report', str(self.root / 'report.json')],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertFalse((self.root / 'report.json').exists())

    def test_existing_output_is_preserved(self):
        self.write('file', elf_header())
        self.output.write_text('keep existing data')
        result = subprocess.run([str(SCANNER), '--firmware-dir', str(self.root),
                                 '--report', str(self.output)],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(self.output.read_text(), 'keep existing data')

    def test_invalid_cli(self):
        for value in ['0', '-1', '100001', 'not-a-number', '10junk']:
            with self.subTest(value=value):
                result = subprocess.run([str(SCANNER), '--firmware-dir', str(self.root),
                                         '--report', str(self.output), '--max-entries', value],
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 2)
                self.assertFalse(self.output.exists())

    def test_random_truncated_inputs_do_not_crash(self):
        rng = random.Random(900)
        for i in range(100):
            magic = b'\x7fELF' if i % 2 else b'\x4f\x15\x3d\x1d'
            self.write(f'{i:03d}.bin', magic + rng.randbytes(rng.randrange(128)))
        _, report = self.scan(1)
        self.assertEqual(len(report['files']), 100)
        self.assertGreater(report['error_count'], 0)

    def test_phase2_segments_entry_tls_and_relocations(self):
        self.write('SceShellUI.elf', analyzed_elf(tls=True, relocations=[(0x400180, 8, 0, 0)]))
        _, report = self.scan()
        elf = report['files'][0]['elf']
        self.assertTrue(elf['entry_in_executable_segment'])
        self.assertTrue(elf['tls_present'])
        self.assertEqual(elf['relocation_types'], {'8': 1})
        self.assertEqual(elf['relocation_status'], 'metadata_only_not_applied')
        self.assertEqual(len(elf['segments']), 3)

    def test_phase2_static_import_export_match(self):
        self.write('SceShellUI.elf', analyzed_elf(needed=['libExample.sprx'],
                   symbols=[('synthetic_function', True, 2, False)]))
        self.write('libExample.sprx', analyzed_elf(soname='libExample.sprx',
                   symbols=[('synthetic_function', False, 2, False)]))
        _, report = self.scan()
        self.assertEqual(report['analysis']['edges'][0]['status'], 'available_metadata_not_loaded')
        self.assertEqual(report['analysis']['imports'][0]['status'], 'matched_lle_export_not_loaded')
        self.assertFalse(report['analysis']['hle_registry_checked'])
        self.assertFalse(report['boot_attempted'])

    def test_phase2_missing_dependency_and_import(self):
        self.write('shell', analyzed_elf(needed=['missing.sprx'], symbols=[('missing', True, 2, False)]))
        _, report = self.scan()
        self.assertEqual(report['analysis']['edges'][0]['status'], 'missing_from_inventory')
        self.assertEqual(report['analysis']['imports'][0]['status'], 'unresolved_static_runtime_unknown')

    def test_phase2_dependency_cycle(self):
        self.write('a.sprx', analyzed_elf(needed=['b.sprx']))
        self.write('b.sprx', analyzed_elf(needed=['a.sprx']))
        _, report = self.scan()
        self.assertEqual(report['analysis']['cycles'], [['a.sprx', 'b.sprx', 'a.sprx']])

    def test_phase2_unrelated_export_does_not_resolve(self):
        self.write('shell', analyzed_elf(symbols=[('same_name', True, 2, False)]))
        self.write('unrelated.sprx', analyzed_elf(symbols=[('same_name', False, 2, False)]))
        _, report = self.scan()
        self.assertEqual(report['analysis']['imports'][0]['status'], 'unresolved_static_runtime_unknown')

    def test_phase2_weak_symbol_is_not_implemented(self):
        self.write('shell', analyzed_elf(symbols=[('optional', True, 2, True)]))
        _, report = self.scan()
        self.assertEqual(report['analysis']['imports'][0]['status'], 'weak_undefined')

    def test_phase2_sce_qualified_ids_match_metadata_names(self):
        self.write('shell', analyzed_elf(sce=True, module='shell', exported_library=None,
                   imported_module='libExampleModule', imported_library='libExample',
                   symbols=[('exampleNidA#C#D', True, 2, False)]))
        self.write('provider.sprx', analyzed_elf(sce=True,
                   symbols=[('exampleNidA#F#H', False, 2, False)]))
        _, report = self.scan()
        symbol = report['analysis']['imports'][0]
        self.assertEqual(symbol['name'], 'exampleNidA')
        self.assertEqual(symbol['library'], 'libExample')
        self.assertEqual(symbol['module'], 'libExampleModule')
        self.assertEqual(symbol['status'], 'matched_lle_export_not_loaded')

    def test_phase2_sce_version_difference(self):
        self.write('shell', analyzed_elf(sce=True, module='shell', exported_library=None,
                   imported_module='libExampleModule', imported_library='libExample',
                   symbols=[('exampleNidA#C#D', True, 2, False)]))
        self.write('provider.sprx', analyzed_elf(sce=True, library_version=2,
                   symbols=[('exampleNidA#F#H', False, 2, False)]))
        _, report = self.scan()
        self.assertIn('version_difference_requires_review', [e['status'] for e in report['analysis']['edges']])
        self.assertNotEqual(report['analysis']['imports'][0]['status'], 'matched_lle_export_not_loaded')

    def test_phase2_bad_relocation_address(self):
        self.write('bad', analyzed_elf(relocations=[(2**64-1, 8, 0, 0)]))
        self.scan(1)

    def test_phase2_bad_relocation_symbol_index(self):
        self.write('bad', analyzed_elf(relocations=[(0x400180, 1, 999, 0)]))
        self.scan(1)

    def test_phase2_bad_alignment(self):
        data = analyzed_elf()
        struct.pack_into('<Q', data, 64 + 48, 3)
        self.write('bad', data)
        self.scan(1)

    def test_phase2_unknown_relocation_recorded_without_application(self):
        self.write('unknown', analyzed_elf(relocations=[(0x400180, 999, 0, 0)]))
        _, report = self.scan()
        self.assertEqual(report['files'][0]['elf']['relocation_types'], {'999': 1})
        self.assertFalse(report['boot_attempted'])

    def test_phase2_vfs_read_only_mapping_and_logs(self):
        path = self.write('modules/file.elf', elf_header())
        original = path.read_bytes()
        result, report = self.scan(extra=('--mount', '/system=modules', '--vfs-path', '/system/file.elf'))
        self.assertIn('[VFS]', result.stdout)
        self.assertEqual(path.read_bytes(), original)
        self.assertTrue(any(d['category'] == 'VFS' and d['result'] == 'allowed' for d in report['diagnostics']))

    def test_phase2_vfs_traversal_rejected(self):
        self.write('file.elf', elf_header())
        _, report = self.scan(1, extra=('--vfs-path', '/../outside'))
        self.assertTrue(any(d['operation'] == 'VFS_open_readonly' and d['result'] == 'denied'
                            for d in report['diagnostics']))

    def test_phase2_vfs_mount_escape_rejected(self):
        self.write('file', b'file')
        self.scan(1, extra=('--mount', '/system=../'))

    @unittest.skipUnless(os.name == 'posix', 'POSIX symlink test')
    def test_phase2_vfs_symlink_escape_rejected(self):
        outside = self.base / 'private'
        outside.mkdir()
        (outside / 'secret').write_text('must not be read')
        self.write('normal', b'data')
        (self.root / 'escape').symlink_to(outside, target_is_directory=True)
        _, report = self.scan(1, extra=('--vfs-path', '/escape/secret'))
        self.assertNotIn('must not be read', self.output.read_text())
        self.assertEqual(len(report['files']), 1)

    def test_phase2_vfs_missing_mapping_directory(self):
        self.write('file', b'data')
        self.scan(1, extra=('--mount', '/system=missing'))

    def test_phase2_reports_are_readable_and_host_root_redacted(self):
        self.write('SceShellUI.elf', analyzed_elf(needed=['missing.sprx']))
        text = self.base / 'report.txt'
        _, report = self.scan(extra=('--text-report', str(text)))
        self.assertIn('missing_from_inventory', text.read_text())
        self.assertNotIn(str(self.root), text.read_text())
        self.assertNotIn(str(self.root), self.output.read_text())
        self.assertEqual(report['schema_version'], 3)

    def test_phase2_text_report_cannot_overwrite_input(self):
        file = self.write('file', b'keep me')
        result = subprocess.run([str(SCANNER), '--firmware-dir', str(self.root),
                                 '--report', str(self.output), '--text-report', str(file)],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(file.read_bytes(), b'keep me')

    def test_phase2_section_dynamic_symbols(self):
        data = analyzed_elf(symbols=[('section_export', False, 2, False)])
        string_size = None
        for offset in range(0x200, 0x1000, 16):
            tag, value = struct.unpack_from('<QQ', data, offset)
            if tag == 10:
                string_size = value
            if tag == 4:
                struct.pack_into('<Q', data, offset, 21)
            if tag == 0:
                break
        struct.pack_into('<Q', data, 40, 0x800)
        struct.pack_into('<HH', data, 58, 64, 3)
        struct.pack_into('<IIQQQQIIQQ', data, 0x840, 0, 11, 0, 0, 0x1200, 48, 2, 0, 8, 24)
        struct.pack_into('<IIQQQQIIQQ', data, 0x880, 0, 3, 0, 0, 0x1000, string_size, 0, 0, 1, 0)
        self.write('section.elf', data)
        _, report = self.scan()
        self.assertEqual(report['files'][0]['symbol_status'], 'parsed_dynamic_symbols')
        self.assertEqual(report['files'][0]['symbols'][0]['name'], 'section_export')

    def test_phase2_protected_provider_is_not_available(self):
        self.write('shell', analyzed_elf(needed=['protected.sprx']))
        self.write('protected.sprx', self_container(flags=2))
        _, report = self.scan()
        self.assertEqual(report['analysis']['edges'][0]['status'], 'protected_self_unavailable')

    def test_phase2_incompatible_machine_provider(self):
        self.write('shell', analyzed_elf(needed=['arm.sprx']))
        self.write('arm.sprx', elf_header(machine=183))
        _, report = self.scan()
        self.assertEqual(report['analysis']['edges'][0]['status'], 'incompatible_file_metadata')

    def test_phase2_bad_sce_qualification_is_not_resolved(self):
        self.write('shell', analyzed_elf(sce=True, module='shell', imported_module='libExampleModule',
                   imported_library='libExample', symbols=[('exampleNidA#Z#Z', True, 2, False)]))
        _, report = self.scan()
        self.assertEqual(report['analysis']['imports'][0]['status'], 'unverified_symbol_qualification')

    def test_phase2_empty_file_is_not_an_executable(self):
        self.write('empty.sprx', b'')
        _, report = self.scan()
        self.assertIsNone(report['files'][0]['elf'])
        self.assertEqual(report['files'][0]['size'], 0)
        self.assertEqual(report['files'][0]['kind'], 'unknown')

    def test_phase2_loading_option_is_unavailable(self):
        result = subprocess.run([str(SCANNER), '--load', 'SceShellUI'],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 2)

    def test_phase2_vfs_oversized_guest_path_is_rejected(self):
        self.write('normal', b'data')
        self.scan(1, extra=('--vfs-path', '/' + 'x' * 4096))

    def test_phase4_explicit_boot_request_fails_closed(self):
        self.write('SceShellUI.synthetic.elf', analyzed_elf())
        result, report = self.scan(1, extra=('--attempt-boot',))
        self.assertFalse(report['boot_attempted'])
        self.assertEqual(report['boot_diagnostic']['state'],
                         'experimental_boot_blocked_before_execution')
        self.assertEqual(report['diagnostics'][-1]['result'], 'boot_backend_unavailable')
        self.assertIn('[PS4_BOOT]', result.stdout)

    def service_input(self, nid, library, module):
        self.write('SceShellCore.synthetic.elf', analyzed_elf(
            sce=True, module='syntheticShell', exported_library=None,
            imported_library=library, imported_module=module,
            symbols=[(nid + '#C#D', True, 2, False)]))

    def test_phase3_services_run_without_boot_mode(self):
        self.write('image', analyzed_elf())
        _, report = self.scan()
        self.assertTrue(report['services']['performed'])
        self.assertFalse(report['boot_diagnostic']['performed'])
        self.assertFalse(report['services']['source_catalog']['runtime_registry'])
        self.assertFalse(report['services']['live_hle_registry_checked'])
        self.assertGreater(report['services']['source_catalog']['entries'], 0)
        self.assertEqual(len(report['services']['source_catalog']['digest_sha256']), 64)

    def test_phase3_shellcore_source_stub_is_partial(self):
        self.service_input('Psl9mfs3duM', 'libSceUserServiceForShellCore', 'libSceUserService')
        result, report = self.scan(extra=('--boot-diagnostic',))
        requirement = report['services']['requirements'][0]
        self.assertEqual(requirement['classification'], 'partially_implemented')
        self.assertEqual(requirement['function_names'], ['sceUserServiceInitializeForShellCore'])
        self.assertFalse(requirement['function_called'])
        self.assertIsNone(requirement['return_code'])
        self.assertTrue(any('userservice.cpp:' in source for source in requirement['source_evidence']))
        events = [event for event in report['diagnostics'] if event['service']]
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]['category'], 'USERSERVICE')
        self.assertEqual(events[0]['service']['requester'], 'SceShellCore.synthetic.elf')
        self.assertIsNone(events[0]['service']['return_code'])
        self.assertIn('[USERSERVICE]', result.stdout)
        self.assertIn('service_source_contains_stub_marker', report['boot_diagnostic']['images'][0]['blockers'])

    def test_phase3_source_registration_is_not_functionality(self):
        self.service_input('QBi7HCK03hw', 'libkernel', 'libkernel')
        _, report = self.scan()
        requirement = report['services']['requirements'][0]
        self.assertEqual(requirement['function_names'], ['sceKernelClockGettime'])
        self.assertEqual(requirement['classification'], 'unknown_or_unverified')
        self.assertEqual(report['services']['classification_counts']['implemented_and_tested'], 0)

    def test_phase3_ipc_stub_is_identified(self):
        self.service_input('HIwEvx4kf6o', 'libSceAppMessaging', 'libSceSystemService')
        result, report = self.scan()
        self.assertIn('[IPC]', result.stdout)
        self.assertEqual(report['services']['requirements'][0]['classification'], 'partially_implemented')

    def test_phase3_wrong_library_identity_is_not_source_match(self):
        self.service_input('Psl9mfs3duM', 'wrongLibrary', 'libSceUserService')
        _, report = self.scan()
        self.assertEqual(report['services']['requirements'][0]['source_evidence'], [])
        self.assertEqual(report['services']['requirements'][0]['classification'], 'unknown_or_unverified')

    def test_phase3_unknown_import_does_not_prove_global_absence(self):
        self.service_input('UnknownNidA', 'libUnknown', 'libUnknown')
        _, report = self.scan()
        requirement = report['services']['requirements'][0]
        self.assertEqual(requirement['classification'], 'unknown_or_unverified')
        self.assertEqual(requirement['nid_validation'], 'syntax_valid_identity_not_authenticated')

    def test_phase3_invalid_nid_length_is_rejected(self):
        self.service_input('short', 'libkernel', 'libkernel')
        _, report = self.scan()
        self.assertEqual(report['analysis']['imports'][0]['status'], 'invalid_nid_syntax')
        self.assertEqual(report['services']['requirements'][0]['source_evidence'], [])

    def test_phase3_invalid_nid_alphabet_is_rejected(self):
        self.service_input('UnknownNid/', 'libkernel', 'libkernel')
        _, report = self.scan()
        self.assertEqual(report['services']['requirements'][0]['nid_validation'], 'invalid_nid_syntax')

    def test_phase3_plain_elf_import_is_not_misclassified_as_nid(self):
        self.write('plain', analyzed_elf(symbols=[('plain_function', True, 2, False)]))
        _, report = self.scan()
        self.assertEqual(report['services']['requirements'][0]['nid_validation'],
                         'not_applicable_unqualified_symbol')

    def test_phase3_lle_export_is_available_metadata_only(self):
        self.write('SceShellUI.synthetic.elf', analyzed_elf(
            needed=['provider.sprx'], symbols=[('function', True, 2, False)]))
        self.write('provider.sprx', analyzed_elf(symbols=[('function', False, 2, False)]))
        _, report = self.scan(extra=('--boot-diagnostic',))
        requirement = report['services']['requirements'][0]
        self.assertEqual(requirement['classification'], 'available_by_lle_metadata')
        self.assertEqual(requirement['provider'], 'provider.sprx')
        self.assertFalse(report['boot_attempted'])
        self.assertFalse(report['boot_diagnostic']['boot_allowed'])

    def test_phase3_boot_plan_memory_tls_and_safe_stop(self):
        self.write('SceShellUI.synthetic.elf', analyzed_elf(tls=True))
        result, report = self.scan(extra=('--boot-diagnostic',))
        boot = report['boot_diagnostic']
        self.assertEqual(boot['state'], 'stopped_safely_without_execution')
        self.assertEqual(boot['module_start_order'], 'not_determined')
        image = boot['images'][0]
        self.assertEqual(image['entry'], 0x400180)
        self.assertEqual(image['segment_bytes'], 0x1800)
        self.assertEqual(image['minimum_page_bytes'], 16384)
        self.assertEqual(image['tls_bytes'], 16)
        self.assertIn('guest_tls_not_allocated', image['blockers'])
        self.assertIn('ps4_process_parameters_missing', image['blockers'])
        self.assertIn('SceShellCore_not_identified_even_by_filename', boot['blockers'])
        self.assertIn('[BOOT]', result.stdout)
        self.assertIn('[PROCESS]', result.stdout)

    def test_phase3_protected_self_never_becomes_bootable(self):
        self.write('SceShellCore.self', self_container(flags=2))
        _, report = self.scan(extra=('--boot-diagnostic',))
        image = report['boot_diagnostic']['images'][0]
        self.assertIn('self_payload_unavailable_for_analysis', image['blockers'])
        self.assertEqual(image['memory_estimate_status'], 'not_inspected')
        self.assertFalse(report['boot_attempted'])

    def test_phase3_invalid_input_still_generates_stopped_diagnostic(self):
        _, report = self.scan(1, root=self.base / 'missing', extra=('--boot-diagnostic',))
        self.assertIn('input_or_analysis_incomplete', report['boot_diagnostic']['blockers'])
        self.assertEqual(report['boot_diagnostic']['state'], 'stopped_safely_without_execution')

    def test_phase3_report_text_redaction_and_readonly_mount(self):
        self.service_input('Psl9mfs3duM', 'libSceUserServiceForShellCore', 'libSceUserService')
        source = self.root / 'SceShellCore.synthetic.elf'
        before = source.read_bytes()
        text_output = self.base / 'boot.txt'
        _, report = self.scan(extra=('--boot-diagnostic', '--text-report', str(text_output),
                                     '--mount', '/system=.', '--vfs-path',
                                     '/system/SceShellCore.synthetic.elf'))
        text = text_output.read_text()
        self.assertIn('PS4 Firmware 9.00 Boot Diagnostic', text)
        self.assertIn('partially_implemented', text)
        self.assertNotIn(str(self.root), text)
        self.assertNotIn(str(self.root), self.output.read_text())
        self.assertEqual(source.read_bytes(), before)
        self.assertTrue(report['services']['complete'])

    def test_phase3_failed_mount_marks_service_analysis_incomplete(self):
        self.write('image', analyzed_elf())
        _, report = self.scan(1, extra=('--boot-diagnostic', '--mount', '/system=../outside'))
        self.assertFalse(report['services']['complete'])
        self.assertIn('input_or_analysis_incomplete', report['boot_diagnostic']['blockers'])

    def test_phase3_log_identity_preserves_same_nid_in_different_libraries(self):
        data = analyzed_elf(sce=True, module='shell', exported_library=None,
                            imported_module='libSceSystemService', imported_library='libSceAppMessaging',
                            symbols=[('HIwEvx4kf6o#C#D', True, 2, False),
                                     ('HIwEvx4kf6o', True, 2, False)])
        self.write('SceShellUI.synthetic.elf', data)
        _, report = self.scan()
        events = [event['service'] for event in report['diagnostics'] if event['service']]
        self.assertEqual(len(events), 2)
        self.assertEqual(events[0]['library'], 'libSceAppMessaging')
        self.assertEqual(events[1]['library'], '')


if __name__ == '__main__':
    unittest.main(argv=[__file__, *remaining], verbosity=2)
