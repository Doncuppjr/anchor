#!/usr/bin/env python3
"""Package pinned CI firmware outputs into an ESP Web Tools static site."""
import argparse
import json
import shutil
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dist', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--version', required=True)
    args = parser.parse_args()
    site = args.output
    site.mkdir(parents=True, exist_ok=True)
    shutil.copy2(Path(__file__).with_name('index.html'), site / 'index.html')
    for variant in ('production', 'debug'):
        source = args.dist / variant
        target = site / variant
        target.mkdir(parents=True, exist_ok=True)
        firmware = f'ANCHOR-C5-{variant}-full.bin'
        data = (source / firmware).read_bytes()
        app = (source / f'ANCHOR-C5-{variant}-app.bin').read_bytes()
        assert len(data) == 8 * 1024 * 1024, 'Unexpected C5 full-flash size'
        assert data[0x2000] == 0xE9, 'Missing C5 bootloader header'
        assert data[0x8000:0x8002] == bytes([0xAA, 0x50]), 'Missing partition table'
        assert data[0x10000:0x10000+len(app)] == app, 'Merged app differs from OTA image'
        shutil.copy2(source / firmware, target / firmware)
        for extra in ('SHA256SUMS', 'BUILD-METADATA.txt'):
            shutil.copy2(source / extra, target / extra)
        manifest = {
            'name': f'ANCHOR C5 {variant}',
            'version': args.version,
            'new_install_prompt_erase': True,
            'new_install_improv_wait_time': 0,
            'builds': [{
                'chipFamily': 'ESP32-C5',
                'improv': False,
                'parts': [{'path': firmware, 'offset': 0}],
            }],
        }
        (target / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print(f'{variant}: {firmware} ({len(data)} bytes), version {args.version}')


if __name__ == '__main__':
    main()
