"""Exercise real IDF parsing, artifact checks and the flash action gate."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts/eidolon'))
import partition_contract as contract


class StorageContractTest(unittest.TestCase):
    def setUp(self):
        self.path = ROOT / 'partitions/v2/16m_eidolon.csv'
        self.table = contract.load_table(self.path)

    def test_supported_layouts(self):
        for name in ('16m_eidolon.csv', '16m_eidolon_box3.csv'):
            contract.require_contract(contract.load_table(self.path.parent / name), 16 * 1024**2)

    def test_bad_contracts(self):
        for mutation in ('missing', 'name', 'type', 'size', 'unequal_ota'):
            with self.subTest(mutation=mutation):
                table = contract.load_table(self.path)
                owner = next(p for p in table if p.name == 'owner_trust')
                if mutation == 'missing': table.remove(owner)
                if mutation == 'name': owner.name = 'wrong_name'
                if mutation == 'type': owner.subtype = 0x82
                if mutation == 'size': owner.size = 0x8000
                if mutation == 'unequal_ota': next(p for p in table if p.name == 'ota_1').size -= 0x1000
                with self.assertRaises(ValueError): contract.require_contract(table, 16 * 1024**2)

    def test_capacity(self):
        with self.assertRaises(Exception): contract.require_contract(self.table, 8 * 1024**2)

    def test_idf_rejects_overlap_duplicate_and_alignment(self):
        for mutation in ('overlap', 'duplicate', 'alignment'):
            table = contract.load_table(self.path)
            if mutation == 'overlap': table[-1].offset = 0x20000
            if mutation == 'duplicate': table[-1].name = 'nvs'
            if mutation == 'alignment': table[-1].offset += 1
            with self.assertRaises(Exception): table.verify()

    def test_device_compatibility(self):
        raw = self.table.to_binary()
        contract.check_existing(raw, self.table, False)
        contract.check_existing(b'\xff' * 4096, self.table, True)
        for raw in (b'\xff' * 4096, b'\x00' * 4096,
                    contract.load_table(self.path.parent / '16m_eidolon_box3.csv').to_binary()):
            with self.assertRaises(ValueError): contract.check_existing(raw, self.table, False)
        old = contract.load_table(self.path)
        old.remove(next(p for p in old if p.name == 'owner_trust'))
        with self.assertRaises(ValueError): contract.check_existing(old.to_binary(), self.table, True)

    def test_artifacts(self):
        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp)
            cfg = build / 'sdkconfig'
            cfg.write_text('CONFIG_EIDOLON_HUB_MODE=y\nCONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y\nCONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/v2/16m_eidolon.csv"\n')
            (build / 'project_description.json').write_text(json.dumps({'config_file': str(cfg)}))
            args = {'flash_settings': {'flash_size': '16MB'},
                    'partition-table': {'offset': '0x8000', 'file': 'table.bin'},
                    'bootloader': {'offset': '0x0', 'file': 'boot.bin'},
                    'flash_files': {'0x8000': 'table.bin', '0x0': 'boot.bin', '0x20000': 'app.bin'}}
            (build / 'flasher_args.json').write_text(json.dumps(args))
            (build / 'table.bin').write_bytes(self.table.to_binary())
            (build / 'boot.bin').write_bytes(b'boot')
            (build / 'app.bin').write_bytes(b'app')
            contract.validate_build(build)
            original_config = cfg.read_text()
            cfg.write_text(original_config.replace('CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y', 'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=n'))
            with self.assertRaisesRegex(ValueError, 'rollback support'):
                contract.validate_build(build)
            cfg.write_text(original_config)
            for reserve in (262143, 60224, 0):
                (build / 'app.bin').write_bytes(b'x' * (0x450000 - reserve))
                with self.assertRaisesRegex(ValueError, 'growth reserve'):
                    contract.validate_build(build)
            (build / 'app.bin').write_bytes(b'x' * (0x450000 - 262144))
            contract.validate_build(build)
            (build / 'app.bin').write_bytes(b'x' * (0x450000 + 1))
            with self.assertRaises(ValueError): contract.validate_build(build)
            (build / 'app.bin').write_bytes(b'app')
            (build / 'table.bin').write_bytes(contract.load_table(self.path.parent / '16m_eidolon_box3.csv').to_binary())
            with self.assertRaises(ValueError): contract.validate_build(build)

    def test_real_device_gate_uses_actual_chip_capacity_and_table(self):
        from unittest.mock import MagicMock
        args = {'extra_esptool_args': {'chip': 'esp32s3'},
                'flash_settings': {'flash_size': '16MB'},
                'partition-table': {'offset': '0x8000'}}
        for case in ('valid', 'wrong-chip', 'small-flash', 'bad-table'):
            esp = MagicMock()
            esp.CHIP_NAME = 'ESP32' if case == 'wrong-chip' else 'ESP32-S3'
            esp.run_stub.return_value = esp
            esp.flash_id.return_value = (0x17 if case == 'small-flash' else 0x18) << 16
            esp.read_flash.return_value = b'bad' if case == 'bad-table' else self.table.to_binary()
            with patch.object(contract, 'validate_build', return_value=(args, self.table, self.path)), \
                 patch('esptool.cmds.detect_chip', return_value=esp):
                if case == 'valid':
                    contract.check_device(Path('/unused'), 'test-port')
                    esp.read_flash.assert_called_once_with(0x8000, 0x1000)
                else:
                    with self.assertRaises(ValueError): contract.check_device(Path('/unused'), 'test-port')
            esp.hard_reset.assert_called_once()
            esp._port.close.assert_called_once()
            esp.write_flash.assert_not_called()

    def test_written_application_selects_actual_slot_for_both_layouts(self):
        for layout in ('16m_eidolon.csv', '16m_eidolon_box3.csv'):
            table = contract.load_table(self.path.parent / layout)
            for name in ('ota_0', 'ota_1'):
                part = next(p for p in table if p.name == name)
                args = {'app': {'offset': hex(part.offset), 'file': 'app.bin'},
                        'flash_files': {hex(part.offset): 'app.bin'}}
                with patch.object(contract, 'validate_build', return_value=(args, table, None)):
                    self.assertEqual(contract.application_partition('/unused'), name)
                    args['flash_files'][hex(part.offset)] = 'other.bin'
                    with self.assertRaises(ValueError):
                        contract.application_partition('/unused')

    def test_activation_delegates_to_official_tool_and_propagates_failure(self):
        import subprocess
        with tempfile.TemporaryDirectory() as temp:
            Path(temp, 'flasher_args.json').write_text(json.dumps({
                'partition-table': {'offset': '0x8000'}}))
            with patch('subprocess.run') as run:
                contract.activate_application(temp, 'test-port', 'ota_1')
                command = run.call_args.args[0]
                self.assertTrue(command[1].endswith('/components/app_update/otatool.py'))
                self.assertEqual(command[-3:], ['switch_ota_partition', '--name', 'ota_1'])
                self.assertTrue(run.call_args.kwargs['check'])
                run.side_effect = subprocess.CalledProcessError(1, command)
                with self.assertRaises(subprocess.CalledProcessError):
                    contract.activate_application(temp, 'test-port', 'ota_1')


if __name__ == '__main__': unittest.main()
