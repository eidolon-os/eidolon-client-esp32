"""Eidolon storage checks, using ESP-IDF's parser and esptool transport.

No CSV parser, partition offsets, flash writer or automatic migration lives here.
"""
import argparse
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]


def parser():
    sys.path.insert(0, str(Path(os.environ['IDF_PATH']) / 'components/partition_table'))
    import gen_esp32part
    return gen_esp32part


def load_table(path, offset=0x8000):
    gen = parser()
    gen.offset_part_table = offset
    with Path(path).open('rb') as f:
        table, _ = gen.PartitionTable.from_file(f)
    table.verify()
    return table


def config(path):
    return dict(line.rstrip().split('=', 1) for line in Path(path).read_text().splitlines()
                if line.startswith('CONFIG_') and '=' in line)


def require_contract(table, flash_bytes):
    gen = parser()
    table.verify_size_fits(flash_bytes)
    # Reuse the firmware's Owner storage name and capacity contract.
    source = (ROOT / 'main/eidolon/owner_trust_storage_policy.h').read_text()
    owner = re.search(r'kOwnerTrustPartitionName\[\] = "([^"]+)"', source)[1]
    kib = int(re.search(r'kOwnerTrustPartitionBytes = (\d+) \* 1024', source)[1])
    requirements = [(owner, 'data', 'nvs', kib * 1024),
                    ('nvs', 'data', 'nvs', 0x3000),
                    ('otadata', 'data', 'ota', 0x2000),
                    ('ota_0', 'app', 'ota_0', 1),
                    ('ota_1', 'app', 'ota_1', 1),
                    ('assets', 'data', 'spiffs', 1)]
    by_name = {p.name: p for p in table}
    for name, kind, subtype, minimum in requirements:
        part = by_name.get(name)
        typ = gen.TYPES[kind]
        if part is None or part.type != typ or part.subtype != gen.SUBTYPES[typ][subtype] or part.size < minimum:
            raise ValueError(f'{name}: requires {kind}/{subtype}, at least {minimum} bytes; select a compatible layout (no automatic relocation)')
    if by_name['ota_0'].size != by_name['ota_1'].size:
        raise ValueError('OTA slots must have equal capacity')


def build_config(build):
    desc = json.loads((build / 'project_description.json').read_text())
    return config(desc['config_file'])


def validate_build(build):
    build = Path(build)
    cfg = build_config(build)
    if cfg.get('CONFIG_EIDOLON_HUB_MODE') != 'y':
        return None
    from esptool.util import flash_size_bytes
    args = json.loads((build / 'flasher_args.json').read_text())
    offset = int(args['partition-table']['offset'], 0)
    table_path = build / args['partition-table']['file']
    table = load_table(table_path, offset)
    require_contract(table, flash_size_bytes(args['flash_settings']['flash_size']))
    csv_path = ROOT / cfg['CONFIG_PARTITION_TABLE_CUSTOM_FILENAME'].strip('"')
    if table.to_binary() != load_table(csv_path, offset).to_binary():
        raise ValueError('Built partition table differs from configured CSV; rebuild before flashing')
    for address, filename in args['flash_files'].items():
        start = int(address, 0)
        size = (build / filename).stat().st_size
        if filename == args['partition-table']['file']:
            limit = 0x1000
        elif filename == args['bootloader']['file']:
            limit = offset - start
        else:
            part = next((p for p in table if p.offset == start), None)
            if part is None:
                raise ValueError(f'{filename}: no partition at write offset {address}')
            limit = part.size
        if size > limit:
            raise ValueError(f'{filename}: {size} bytes exceeds capacity {limit}')
    return args, table, table_path


def check_existing(raw, expected, allow_blank):
    if all(b == 0xff for b in raw):
        if allow_blank:
            return
        raise ValueError('Device has no partition table: use a complete flash, not a partial flash')
    try:
        actual = parser().PartitionTable.from_binary(raw)
        actual.verify()
    except Exception as error:
        raise ValueError('Device partition table unreadable; refusing to overwrite ownership/data') from error
    if actual.to_binary() != expected.to_binary():
        raise ValueError('Device layout differs: stop and plan migration or explicitly authorized full erase; no automatic erase')


def check_device(build, port, allow_blank=False, after=False):
    validated = validate_build(build)
    if validated is None:
        return
    args, table, _ = validated
    from esptool.cmds import detect_chip, DETECTED_FLASH_SIZES
    from esptool.util import flash_size_bytes
    esp = detect_chip(port, connect_attempts=3)
    try:
        target = esp.CHIP_NAME.lower().replace('-', '')
        if target != args['extra_esptool_args']['chip']:
            raise ValueError(f'Connected chip {target} differs from build target')
        esp = esp.run_stub()
        capacity = DETECTED_FLASH_SIZES.get((esp.flash_id() >> 16) & 0xff)
        if capacity is None or flash_size_bytes(capacity) < flash_size_bytes(args['flash_settings']['flash_size']):
            raise ValueError('Actual flash capacity is unknown or smaller than configured flash')
        raw = esp.read_flash(int(args['partition-table']['offset'], 0), 0x1000)
        check_existing(raw, table, allow_blank and not after)
        print('EIDOLON partition contract: ' + ('readback verified' if after else 'preflight passed'))
    finally:
        try:
            esp.hard_reset()
        finally:
            esp._port.close()


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    sub = cli.add_subparsers(dest='command', required=True)
    csv = sub.add_parser('csv')
    csv.add_argument('path')
    csv.add_argument('--flash-bytes', type=lambda s: int(s, 0), required=True)
    csv.add_argument('--offset', type=lambda s: int(s, 0), default=0x8000)
    field = sub.add_parser('field')
    field.add_argument('path')
    field.add_argument('name')
    field.add_argument('field', choices=['offset', 'size'])
    built = sub.add_parser('build')
    built.add_argument('path')
    args = cli.parse_args()
    if args.command == 'csv':
        require_contract(load_table(args.path, args.offset), args.flash_bytes)
    elif args.command == 'field':
        part = next((p for p in load_table(args.path) if p.name == args.name), None)
        if part is None:
            raise ValueError(f'Unknown partition: {args.name}')
        print(hex(getattr(part, args.field)))
    else:
        validate_build(Path(args.path))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        sys.exit(f'EIDOLON partition contract: {error}')
