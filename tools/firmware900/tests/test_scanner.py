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


if __name__ == '__main__':
    unittest.main(argv=[__file__, *remaining], verbosity=2)
