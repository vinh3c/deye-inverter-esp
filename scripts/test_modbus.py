"""Protocol and decoding tests use a loopback fake logger, never the inverter."""
from contextlib import contextmanager
import json
from pathlib import Path
import shutil
import socketserver
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import modbus
import generate_registers


WORDS = {90: 1500, 91: 1400, 182: 1300, 184: 83, 109: 3800, 110: 25,
         111: 4000, 112: 30, 186: 940, 187: 1190, 150: 2300, 160: 65486,
         169: 65436, 178: 1800, 157: 2300, 179: 800, 190: 65036, 183: 5120, 191: 64536}


@contextmanager
def logger(mode="normal", fail_address=None):
    requests = []

    class Handler(socketserver.BaseRequestHandler):
        def handle(self):
            request = b""
            while len(request) < 12:
                data = self.request.recv(12 - len(request))
                if not data:
                    return
                request += data
            transaction, protocol, length, unit, function, address, count = struct.unpack(">HHHBBHH", request)
            requests.append((address, count, function, protocol, length))
            body = bytes([3, count * 2]) + struct.pack(f">{count}H", *(WORDS.get(address + i, 0) for i in range(count)))
            if mode == "exception" or address == fail_address:
                body = b"\x83\x02"
            if mode == "byte_count":
                body = b"\x03\x00" + body[2:]
            if mode == "transaction":
                transaction += 1
            if mode == "protocol":
                protocol = 1
            if mode == "unit":
                unit = 2
            length = len(body) + 1
            if mode == "length":
                length = 65535
            response = struct.pack(">HHHB", transaction, protocol, length, unit) + body
            if mode == "truncated":
                self.request.sendall(response[:8])
            elif mode == "timeout":
                time.sleep(.1)
            elif mode == "fragmented":
                for part in (response[:1], response[1:5], response[5:8], response[8:]):
                    self.request.sendall(part)
                    time.sleep(.005)
            else:
                self.request.sendall(response)

    with socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler) as server:
        server.daemon_threads = True
        thread = threading.Thread(target=lambda: server.serve_forever(poll_interval=.01), daemon=True)
        thread.start()
        try:
            yield modbus.Client("127.0.0.1", server.server_address[1]), requests
        finally:
            server.shutdown()
            thread.join()


class ModbusTests(unittest.TestCase):
    def test_invalid_connection_options_fail_before_network_access(self):
        for port, unit, timeout in ((0, 1, 3), (65536, 1, 3), (502, 0, 3),
                                    (502, 248, 3), (502, 1, float('nan')), (502, 1, float('inf'))):
            with self.subTest(port=port, unit=unit, timeout=timeout), patch.object(modbus.socket, "create_connection") as connect:
                with self.assertRaises(ValueError):
                    modbus.Client("127.0.0.1", port, unit, timeout)
                connect.assert_not_called()

    def test_full_snapshot_uses_six_sequential_compact_fc03_reads(self):
        with logger("fragmented") as (client, requests):
            reading = modbus.poll(client)
        self.assertTrue(reading["ok"])
        self.assertEqual([(address, count) for address, count, *_ in requests], [tuple(g) for g in modbus.PROFILE["groups"]])
        self.assertTrue(all((function, protocol, length) == (3, 0, 6) for _, _, function, protocol, length in requests))
        self.assertEqual(sum(count for _, count, *_ in requests), 26)
        for key, expected in {"acC": 40, "dcC": 50, "batteryC": 30, "batteryPercent": 83,
                              "pv1VoltageV": 380, "pv1CurrentA": 2.5, "pv1PowerW": 940,
                              "pv2VoltageV": 400, "pv2PowerW": 1190, "gridPowerW": -100,
                              "gridCurrentA": -.5, "loadPowerW": 1800, "batteryPowerW": -500,
                              "batteryVoltageV": 51.2, "batteryCurrentA": -10}.items():
            self.assertEqual(reading[key], expected, key)
        self.assertNotEqual(reading["pv1PowerW"], reading["pv1VoltageV"] * reading["pv1CurrentA"])

    def test_bad_responses_are_rejected(self):
        for mode in ("exception", "transaction", "protocol", "unit", "length", "byte_count", "truncated", "timeout"):
            with self.subTest(mode=mode), logger(mode) as (client, _):
                client.timeout = .05
                with self.assertRaises((OSError, modbus.ModbusError)):
                    client.read(90, 2)

    def test_group_failure_preserves_independent_attributes_and_next_cycle_recovers(self):
        with logger(fail_address=150) as (client, requests):
            reading = modbus.poll(client)
        self.assertEqual(len(requests), 6)
        self.assertFalse(reading["ok"])
        self.assertIsNone(reading["gridVoltageV"])
        self.assertEqual(reading["gridPowerW"], -100)
        self.assertEqual(reading["acC"], 40)
        with logger() as (client, _):
            self.assertTrue(modbus.poll(client)["ok"])

    def test_signed_minus_one_unsigned_sentinel_zero_and_temperature_offset(self):
        metrics = {m["key"]: m for m in modbus.PROFILE["metrics"]}
        for key in ("gridPowerW", "loadPowerW", "batteryPowerW"):
            self.assertEqual(modbus.decode(metrics[key], 65535), -1)
        self.assertEqual(modbus.decode(metrics["batteryCurrentA"], 65535), -.01)
        self.assertIsNone(modbus.decode(metrics["pv1PowerW"], 65535))
        self.assertIsNone(modbus.decode(metrics["batteryPercent"], 101))
        self.assertEqual(modbus.decode(metrics["acC"], 950), -5)
        self.assertEqual(modbus.decode(metrics["pv1PowerW"], 0), 0)
        self.assertIsNone(modbus.decode(metrics["acC"], 3001))

    def test_grid_only_and_range_limit(self):
        with logger() as (client, requests):
            result = modbus.poll(client, True)
            self.assertEqual(result["gridVoltageV"], 230)
            self.assertNotIn("acC", result)
            for address, count in ((0, 126), (65535, 2), (-1, 1), (1, 0)):
                with self.assertRaises(ValueError):
                    client.read(address, count)
        self.assertEqual(len(requests), 1)

    def test_jsonl_log_includes_raw_registers_timing_and_finite_count(self):
        import argparse
        with tempfile.TemporaryDirectory() as directory, logger() as (client, requests):
            path = Path(directory) / "log.jsonl"
            args = argparse.Namespace(host=client.host, logger_port=client.port, unit_id=1,
                timeout=1, count=2, interval=.1, watch=False, log=path, grid_only=False, json=False)
            with patch.object(modbus, "display"):
                modbus.run_read(args, (client.host, client.port))
            rows = [json.loads(line) for line in path.read_text().splitlines()]
        self.assertEqual(len(rows), 2)
        self.assertEqual(len(requests), 12)
        self.assertEqual(rows[0]["raw"]["batteryPowerW"], 65036)
        self.assertEqual(rows[0]["registers"]["batteryPowerW"], 190)
        self.assertGreaterEqual(rows[0]["elapsed_ms"], 0)

    def test_native_firmware_decoder_and_protocol(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Host C++ compiler unavailable; ESP firmware build still required")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            generate_registers.generate(modbus.ROOT / "profiles/deye-single.json", output / "deye_registers.h")
            executable = output / "modbus-test.exe"
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(modbus.ROOT / "main"), "-I", str(output),
                            str(modbus.ROOT / "scripts/modbus_core_test.cpp"), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
