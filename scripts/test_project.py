"""Checks for native tooling; never accesses SDK downloads or serial hardware."""
import json
from pathlib import Path
import tempfile
import subprocess
import unittest
from unittest.mock import patch

import project
import native_args
import configure_logger


class LoggerConfigTests(unittest.TestCase):
    def test_valid_host_and_port(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            for host in ("192.168.5.190", "deye-logger.local"):
                path.write_text(json.dumps({"logger": {"host": host, "port": 502}}))
                self.assertEqual(project.load_logger_config(path), (host, 502))

    def test_invalid_config_is_rejected_before_flash(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            cases = [None, {"logger": []}, {"logger": {"host": "x"}},
                     {"logger": {"host": "x", "port": 502}, "wifi": {} }]
            for host, port in (("http://logger", 502), ("x\r\nmatter esp factoryreset", 502),
                               ("a" * 64, 502), ("", 502), ("x", True), ("x", 0),
                               ("x", 65536), ("x", "502")):
                cases.append({"logger": {"host": host, "port": port}})
            for data in cases:
                path.write_text(json.dumps(data))
                with self.subTest(data=data), patch.object(project, "flash") as flash:
                    with patch("sys.argv", ["fw", "flash", "--port", "COM99", "--config", str(path)]):
                        with self.assertRaises(ValueError):
                            project.main()
                    flash.assert_not_called()

    def test_serial_ack_requires_fresh_token_and_complete_line(self):
        from unittest.mock import Mock
        port = Mock()
        port.read.side_effect = [
            b"matter esp logger set logger.local 502 12345678\r\n",
            b"LOGGER_CONFIG_SAVED token=aaaaaaaa host=logger.local port=502\r\n",
            b"LOGGER_CONFIG_SAVED token=12345678 host=logger.local port=502",
            b"\r\n",
        ]
        ticks = iter(range(100))
        with patch.object(configure_logger.time, "monotonic", side_effect=lambda: next(ticks)):
            configure_logger.apply_config(port, "logger.local", 502, token="12345678")
        self.assertEqual(port.read.call_count, 4)
        port.write.assert_called_with(b"matter esp logger set logger.local 502 12345678\r\n")

    def test_flash_applies_local_config_only_when_present(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for present in (False, True):
                if present:
                    (root / "config.local.json").write_text(json.dumps({
                        "logger": {"host": "192.168.5.190", "port": 502}}))
                with self.subTest(present=present), patch.object(project, "ROOT", root), \
                        patch.object(project, "flash") as flash, \
                        patch.object(project, "configure_logger") as configure, \
                        patch("sys.argv", ["fw", "flash", "--port", "COM99"]):
                    project.main()
                    flash.assert_called_once()
                    if present:
                        configure.assert_called_once_with("COM99", ("192.168.5.190", 502))
                    else:
                        configure.assert_not_called()

    def test_config_only_skips_build_and_flash(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "settings.json"
            path.write_text(json.dumps({"logger": {"host": "logger.local", "port": 1502}}))
            with patch.object(project, "flash") as flash, patch.object(project, "idf") as idf, \
                    patch.object(project, "configure_logger") as configure, \
                    patch("sys.argv", ["fw", "config", "--port", "COM99", "--config", str(path)]):
                project.main()
                flash.assert_not_called()
                idf.assert_not_called()
                configure.assert_called_once_with("COM99", ("logger.local", 1502))

    def test_missing_ack_is_a_failure(self):
        from unittest.mock import Mock
        port = Mock()
        port.read.return_value = b"Error: unknown command\n"
        ticks = iter(range(100))
        with patch.object(configure_logger.time, "monotonic", side_effect=lambda: next(ticks)):
            with self.assertRaisesRegex(RuntimeError, "No config commit acknowledgement"):
                configure_logger.apply_config(port, "logger.local", 502, timeout=10)


class CompileFlagsTests(unittest.TestCase):
    def test_quotes_and_backslashes_survive_gn_encoding(self):
        values = ['-DNAME="hello"', r'-IC:\sdk\include', '-DPRICE=$1']
        encoded = native_args.gn_list(values)
        self.assertIn(r'\"hello\"', encoded)
        self.assertIn(r'C:\\sdk\\include', encoded)
        self.assertIn(r'\$1', encoded)

    def test_flags_exclude_output_and_source_and_make_sdk_headers_system(self):
        commands = [{"file": "/sdk/chip/chip.c", "arguments": [
            "gcc", "-I/sdk/idf/components", '-DNAME="hello world"', "-DHAVE_CONFIG_H",
            "-o", "chip.obj", "-c", "/sdk/chip/chip.c",
        ]}]
        self.assertEqual(native_args.flags_for(commands, "/sdk/chip/chip.c", "/sdk/idf", ["-DHAVE_CONFIG_H"]),
                         ["-isystem/sdk/idf/components", '-DNAME="hello world"'])

    def test_command_line_parser_keeps_quoted_definition(self):
        command = subprocess.list2cmdline(["gcc", '-DNAME="hello world"', "-c", "chip.c"]) if project.WINDOWS else 'gcc \'-DNAME="hello world"\' -c chip.c'
        self.assertEqual(native_args.split_command(command), ["gcc", '-DNAME="hello world"', "-c", "chip.c"])


@unittest.skipUnless(project.WINDOWS, "Windows junction handling")
class JunctionTests(unittest.TestCase):
    def test_link_target_can_pass_through_another_placeholder(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            (root / "target/headers").mkdir(parents=True)
            (root / "target/headers/a.h").write_text("header")
            (root / "a").write_text("z/headers")
            (root / "z").write_text("target")
            def git_output(args, **kwargs):
                if "--stage" in args:
                    return "120000 abc 0\ta\0" + "120000 def 0\tz\0"
                return "z/headers" if args[-1] == "HEAD:a" else "target"
            with patch.object(project, "SDK", root), patch.object(project, "run", side_effect=git_output):
                project.windows_links(root, {})
            self.assertEqual((root / "a/a.h").read_text(), "header")

    def test_placeholder_becomes_junction_and_can_be_processed_twice(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            (root / "target").mkdir()
            (root / "target/header.h").write_text("header")
            (root / "alias").write_text("target")
            def git_output(args, **kwargs):
                return "120000 abc 0\talias\0" if "--stage" in args else "target"
            with patch.object(project, "SDK", root), patch.object(project, "run", side_effect=git_output):
                project.windows_links(root, {})
                project.windows_links(root, {})
            self.assertEqual((root / "alias/header.h").read_text(), "header")


class FlashTests(unittest.TestCase):
    def test_wrong_target_is_rejected_before_opening_device(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "flasher_args.json").write_text(json.dumps({
                "write_flash_args": [], "flash_files": {},
                "extra_esptool_args": {"chip": "esp32s3"},
            }))
            with patch.object(project, "serial_command") as serial:
                with self.assertRaisesRegex(RuntimeError, "Firmware is for esp32s3"):
                    project.flash(root, "COM99", 460800, target="esp32-c3")
                serial.assert_not_called()

    def test_flash_uses_manifest_offsets_and_options(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "bootloader").mkdir()
            (root / "bootloader/bootloader.bin").write_bytes(b"boot")
            (root / "app.bin").write_bytes(b"app")
            (root / "flasher_args.json").write_text(json.dumps({
                "write_flash_args": ["--flash_mode", "dio", "--flash_size", "4MB"],
                "flash_files": {"0x0": "bootloader/bootloader.bin", "0x123000": "app.bin"},
            }))
            with patch.object(project, "serial_command") as serial:
                project.flash(root, "COM99", 460800)
            module, args = serial.call_args.args
            self.assertEqual(module, "esptool")
            self.assertIn("COM99", args)
            self.assertEqual(args[args.index("0x123000") + 1], str((root / "app.bin").resolve()))
            self.assertEqual(args[args.index("--flash_mode") + 1], "dio")

    def test_missing_binary_fails_before_opening_device(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "flasher_args.json").write_text(json.dumps({
                "write_flash_args": [], "flash_files": {"0x0": "missing.bin"},
            }))
            with patch.object(project, "serial_command") as serial:
                with self.assertRaisesRegex(RuntimeError, "Missing or invalid"):
                    project.flash(root, "COM99", 460800)
                serial.assert_not_called()

    def test_manifest_cannot_flash_a_file_outside_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "outside.bin").write_bytes(b"outside")
            build = root / "artifact"
            build.mkdir()
            (build / "flasher_args.json").write_text(json.dumps({
                "write_flash_args": [], "flash_files": {"0x0": "../outside.bin"},
            }))
            with patch.object(project, "serial_command") as serial:
                with self.assertRaisesRegex(RuntimeError, "Missing or invalid"):
                    project.flash(build, "COM99", 460800)
                serial.assert_not_called()


if __name__ == "__main__":
    unittest.main()
