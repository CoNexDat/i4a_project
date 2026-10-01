import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "auxiliar"))
import esp_rpi_flasher as flasher
from export_firmware_bundle import export_bundle


class BundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / "build"
        self.output = self.root / "bundle with spaces"
        self.build.mkdir()
        self.images = {"0x1000": "bootloader/bootloader.bin",
                       "0x8000": "partition_table/partition-table.bin",
                       "0xf000": "ota_data_initial.bin", "0x20000": "main.bin"}
        for offset, name in self.images.items():
            p = self.build / name
            p.parent.mkdir(exist_ok=True)
            p.write_bytes((offset + name).encode())
        (self.build / "flash_args").write_text(
            "--flash_mode dio --flash_freq 40m --flash_size 4MB\n" +
            "\n".join(f"{offset} {name}" for offset, name in self.images.items()),
            encoding="utf-8")

    def test_export_preserves_every_partition_and_checksums(self):
        export_bundle(self.build, self.output)
        _, options, images = flasher.parse_flash_args_tokens(self.output / "flash_args")
        self.assertIn("4MB", options)
        self.assertEqual([offset for offset, _ in images], list(self.images))
        manifest = json.loads((self.output / "manifest.json").read_text())
        for item in manifest["images"]:
            original = (self.build / item["path"]).read_bytes()
            self.assertEqual((self.output / item["path"]).read_bytes(), original)
            self.assertEqual(item["sha256"], hashlib.sha256(original).hexdigest())

    def test_flash_command_uses_generated_offsets_including_ota_data(self):
        export_bundle(self.build, self.output)
        args = flasher.build_parser().parse_args([
            "flash", "--port", "COM7", "--flash-args", str(self.output / "flash_args")])
        with patch.object(flasher, "run_command") as run:
            self.assertEqual(flasher.command_flash(args), 0)
        command = run.call_args.args[0]
        for offset, name in self.images.items():
            self.assertEqual(command[command.index(offset) + 1], str(self.output / name))
        self.assertNotIn("0x10000", command)

    def test_missing_image_refuses_export_before_creating_output(self):
        (self.build / "ota_data_initial.bin").unlink()
        with self.assertRaises(flasher.FlasherError):
            export_bundle(self.build, self.output)
        self.assertFalse(self.output.exists())

    def test_existing_bundle_is_not_overwritten(self):
        export_bundle(self.build, self.output)
        with self.assertRaises(flasher.FlasherError):
            export_bundle(self.build, self.output)

    def test_external_image_is_rejected(self):
        (self.root / "outside.bin").write_bytes(b"outside")
        (self.build / "flash_args").write_text("0x20000 ../outside.bin\n")
        with self.assertRaises(flasher.FlasherError):
            export_bundle(self.build, self.output)
        self.assertFalse(self.output.exists())

    def test_missing_image_prevents_erase_and_flash(self):
        export_bundle(self.build, self.output)
        (self.output / "main.bin").unlink()
        args = flasher.build_parser().parse_args([
            "flash", "--port", "COM7", "--erase-first",
            "--flash-args", str(self.output / "flash_args")])
        with patch.object(flasher, "run_command") as run:
            with self.assertRaises(flasher.FlasherError):
                flasher.command_flash(args)
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
