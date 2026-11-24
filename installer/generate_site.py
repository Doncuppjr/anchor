#!/usr/bin/env python3
"""Generate per-board ESP Web Tools manifests from validated firmware artifacts."""
import argparse
import html
import json
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROFILES = ('production', 'debug')


def board_config(board):
    if not re.fullmatch(r'[a-z0-9][a-z0-9-]*', board):
        raise ValueError(f'Invalid board identifier {board!r}')
    path = ROOT / 'boards' / f'{board}.mk'
    if not path.is_file():
        raise ValueError(f'Unknown board {board}: {path} missing')
    values = {}
    for line in path.read_text().splitlines():
        match = re.fullmatch(r'([A-Z_]+)\s*:=\s*(.*?)\s*', line)
        if match:
            values[match.group(1)] = match.group(2)
    required = ('BOARD_NAME', 'BOARD_IMAGE_PREFIX', 'BOARD_FULL_SIZE',
                'BOARD_OTA_SLOT', 'BOARD_CHIP_FAMILY', 'BOARD_BOOT_OFFSET',
                'BOARD_INSTALL_STATUS')
    for key in required:
        if key not in values:
            raise ValueError(f'{path}: {key} missing')
    return values


def build_site(dist, output, version, boards):
    if not boards or len(set(boards)) != len(boards):
        raise ValueError('At least one unique board is required')
    config = {board: board_config(board) for board in boards}
    output.mkdir(parents=True, exist_ok=True)
    options = []
    for board in boards:
        cfg = config[board]
        options.append(f'<option value="{html.escape(board)}">{html.escape(cfg["BOARD_NAME"])}</option>')
        for variant in PROFILES:
            source = dist / board / variant
            target = output / board / variant
            target.mkdir(parents=True, exist_ok=True)
            prefix = cfg['BOARD_IMAGE_PREFIX']
            firmware = f'{prefix}-{variant}-full.bin'
            image = (source / firmware).read_bytes()
            app = (source / f'{prefix}-{variant}-app.bin').read_bytes()
            expected_length = int(cfg['BOARD_FULL_SIZE'], 0)
            boot_offset = int(cfg['BOARD_BOOT_OFFSET'], 0)
            max_app = int(cfg['BOARD_OTA_SLOT'], 0)
            assert len(image) == expected_length, f'{board}: incorrect full-flash size'
            assert 0 < len(app) <= max_app, f'{board}: application exceeds OTA slot'
            assert image[boot_offset] == 0xe9, f'{board}: missing bootloader header'
            assert image[0x8000:0x8002] == bytes((0xaa, 0x50)), f'{board}: invalid partition table'
            assert image[0x10000:0x10000 + len(app)] == app, f'{board}: app mismatch'
            shutil.copy2(source / firmware, target / firmware)
            for extra in ('SHA256SUMS', 'BUILD-METADATA.txt'):
                shutil.copy2(source / extra, target / extra)
            manifest = {
                'name': f'Anchor {cfg["BOARD_NAME"]} {variant}',
                'version': version,
                'new_install_prompt_erase': True,
                'new_install_improv_wait_time': 0,
                'builds': [{
                    'chipFamily': cfg['BOARD_CHIP_FAMILY'],
                    'improv': False,
                    'parts': [{'path': firmware, 'offset': 0}],
                }],
            }
            (target / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
            print(f'{board}/{variant}: {firmware} ({len(image)} bytes), version {version}')
    page = Path(__file__).with_name('index.html').read_text()
    page = page.replace('<!-- BOARD_OPTIONS -->', '\n'.join(options))
    (output / 'index.html').write_text(page)
    (output / 'boards.json').write_text(json.dumps({board: {
        'name': cfg['BOARD_NAME'],
        'chipFamily': cfg['BOARD_CHIP_FAMILY'],
        'status': cfg['BOARD_INSTALL_STATUS'],
        'experimental': board != 'c5',
    } for board, cfg in config.items()}, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dist', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--boards', nargs='+', required=True)
    args = parser.parse_args()
    build_site(args.dist, args.output, args.version, args.boards)


if __name__ == '__main__':
    main()
