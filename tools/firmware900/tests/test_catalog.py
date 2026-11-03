# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    'catalog', Path(__file__).resolve().parents[1] / 'generate_service_catalog.py')
catalog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalog)


class CatalogTests(unittest.TestCase):
    def test_comments_and_strings_are_not_registrations(self):
        text = '''// LIB_FUNCTION("FakeNidAAAA", "lib", 1, "mod", fake);
        /* LIB_FUNCTION("FakeNidAAAA", "lib", 1, "mod", fake); */
        const char* text = "LIB_FUNCTION(\\"FakeNidAAAA\\", \\"lib\\", 1, \\"mod\\", fake)";
        LIB_FUNCTION("ExampleNidA", "lib", 1, "mod", real);
        '''
        records, skipped = catalog.registrations(text, 'synthetic.cpp')
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0][4], 'real')
        self.assertEqual(skipped, 0)

    def test_nonliteral_macros_are_reported_as_skipped(self):
        records, skipped = catalog.registrations(
            'LIB_FUNCTION(NID, "lib", VERSION, "mod", function);', 'synthetic.cpp')
        self.assertEqual(records, [])
        self.assertEqual(skipped, 1)

    def test_stub_marker_is_scoped_to_function_body(self):
        text = '''int PS4_SYSV_ABI stub() { LOG_ERROR(log, "(STUBBED) {}", 1); return 0; }
        int PS4_SYSV_ABI other() { return 0; }
        LIB_FUNCTION("ExampleNidA", "lib", 1, "mod", stub);
        LIB_FUNCTION("AnotherNidA", "lib", 1, "mod", other);'''
        records, _ = catalog.registrations(text, 'synthetic.cpp')
        self.assertEqual(records[0][-1], 'source_contains_stub_marker')
        self.assertEqual(records[1][-1], 'source_registration_behavior_unverified')

    def test_forward_declaration_is_not_a_stub(self):
        text = '''int PS4_SYSV_ABI declared();
        int PS4_SYSV_ABI stub() { /* (STUBBED) */ return 0; }
        LIB_FUNCTION("ExampleNidA", "lib", 1, "mod", declared);
        LIB_FUNCTION("AnotherNidA", "lib", 1, "mod", stub);'''
        records, _ = catalog.registrations(text, 'synthetic.cpp')
        self.assertTrue(all('unverified' in record[-1] for record in records))

    def test_generation_is_deterministic_and_digest_tracks_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'synthetic.cpp'
            source.write_text('LIB_FUNCTION("ExampleNidA", "lib", 1, "mod", function);')
            first = catalog.generate(root, [source])
            self.assertEqual(first, catalog.generate(root, [source]))
            self.assertNotIn(directory, first)
            source.write_text(source.read_text() + '\n')
            self.assertNotEqual(first, catalog.generate(root, [source]))


if __name__ == '__main__':
    unittest.main(verbosity=2)
