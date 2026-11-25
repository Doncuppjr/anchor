#!/usr/bin/env python3
"""Static installer regression tests (run after local board builds)."""
import json
import hashlib
import tempfile
import unittest
from pathlib import Path
from installer.generate_site import board_config, build_site

ROOT = Path(__file__).resolve().parents[1]


class InstallerTest(unittest.TestCase):
    def test_board_profiles(self):
        self.assertEqual(board_config('c5')['BOARD_CHIP_FAMILY'], 'ESP32-C5')
        self.assertEqual(board_config('c3-supermini')['BOARD_CHIP_FAMILY'], 'ESP32-C3')
        with self.assertRaises(ValueError):
            board_config('../other')

    def test_site(self):
        with tempfile.TemporaryDirectory() as temporary:
            site = Path(temporary)
            build_site(ROOT / 'dist', site, 'test-version', ['c5', 'c3-supermini'])
            html = (site / 'index.html').read_text()
            self.assertIn('value="c5"', html)
            self.assertIn('value="c3-supermini"', html)
            self.assertNotIn('<!-- BOARD_OPTIONS -->', html)
            self.assertIn('id="uart-update"', html)
            for asset in ('uart-flasher.mjs', 'uart-update.mjs'):
                self.assertTrue((site / asset).is_file())

            self.assertIn("'./' + board + '/' + profile + '/manifest.json'", html)
            boards = json.loads((site / 'boards.json').read_text())
            self.assertTrue(boards['c3-supermini']['experimental'])
            self.assertFalse(boards['c5']['experimental'])
            for board, family, size in [('c5', 'ESP32-C5', 8388608),
                                        ('c3-supermini', 'ESP32-C3', 4194304)]:
                for variant in ('production', 'debug'):
                    path = site / board / variant
                    manifest = json.loads((path / 'manifest.json').read_text())
                    self.assertEqual(manifest['builds'][0]['chipFamily'], family)
                    image = path / manifest['builds'][0]['parts'][0]['path']
                    self.assertEqual(image.stat().st_size, size)
                    self.assertEqual(manifest['version'], 'test-version')
                    full_meta = json.loads((path / 'full.json').read_text())
                    self.assertEqual(full_meta['board'], board)
                    self.assertEqual(full_meta['profile'], variant)
                    self.assertEqual(full_meta['address'], 0)
                    self.assertEqual(full_meta['size'], size)
                    self.assertEqual(full_meta['sha256'], hashlib.sha256(image.read_bytes()).hexdigest())

                    ota = json.loads((path / 'ota.json').read_text())
                    app = (path / ota['path']).read_bytes()
                    self.assertEqual(ota['board'], board)
                    self.assertEqual(ota['profile'], variant)
                    self.assertEqual(ota['chipFamily'], family)
                    self.assertEqual(ota['size'], len(app))
                    self.assertEqual(ota['sha256'], hashlib.sha256(app).hexdigest())
                    self.assertEqual(app[0], 0xe9)


if __name__ == '__main__':
    unittest.main()
