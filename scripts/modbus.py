"""Read-only Deye single-phase Modbus TCP diagnostics. Standard library only."""
import json
import math
from pathlib import Path
import socket
import struct
import time
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parent.parent
PROFILE = json.loads((ROOT / "profiles/deye-single.json").read_text(encoding="utf-8"))


class ModbusError(RuntimeError):
    pass


def receive_exact(connection, size, deadline):
    data = bytearray()
    while len(data) < size:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise ModbusError("Modbus response deadline exceeded")
        connection.settimeout(remaining)
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise ModbusError("Logger closed connection before response was complete")
        data.extend(chunk)
    return bytes(data)


class Client:
    def __init__(self, host, port=502, unit_id=1, timeout=3):
        if not 1 <= port <= 65535 or not 1 <= unit_id <= 247 or not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("TCP port must be 1..65535, unit-id 1..247, and timeout finite and positive")
        self.host, self.port, self.unit_id, self.timeout = host, port, unit_id, timeout
        self.transaction = 0

    def read(self, address, count):
        if not (0 <= address < 65536 and 1 <= count <= 125 and address + count <= 65536):
            raise ValueError("Invalid FC03 register range")
        self.transaction = (self.transaction + 1) & 0xffff
        transaction = self.transaction
        deadline = time.monotonic() + self.timeout
        request = struct.pack(">HHHBBHH", transaction, 0, 6, self.unit_id, 3, address, count)
        with socket.create_connection((self.host, self.port), timeout=self.timeout) as connection:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ModbusError("Modbus connection deadline exceeded")
            connection.settimeout(remaining)
            connection.sendall(request)
            header = receive_exact(connection, 7, deadline)
            reply_id, protocol, length, unit = struct.unpack(">HHHB", header)
            if reply_id != transaction or protocol != 0 or unit != self.unit_id:
                raise ModbusError("Invalid Modbus transaction/protocol/unit ID")
            if not 3 <= length <= 254:
                raise ModbusError("Invalid MBAP length")
            body = receive_exact(connection, length - 1, deadline)
            if body[0] == 0x83:
                raise ModbusError(f"FC03 exception 0x{body[1]:02x} at register {address}")
            if body[0] != 3 or body[1] != count * 2 or length != 3 + count * 2:
                raise ModbusError("Invalid FC03 function/byte count/length")
            return list(struct.unpack(f">{count}H", body[2:]))


def decode(metric, raw):
    if raw is None or not 0 <= raw <= metric.get("maximum_raw", 65535):
        return None
    signed = metric.get("signed", False)
    if not signed and raw == 65535:
        return None
    word = raw - 65536 if signed and raw >= 32768 else raw
    return (word * metric["multiplier"] + metric.get("offset", 0)) / metric["divisor"]


def poll(client, grid_only=False):
    started = time.monotonic()
    raw, groups = {}, []
    for address, count in ([[150, 1]] if grid_only else PROFILE["groups"]):
        begin = time.monotonic()
        group = {"address": address, "count": count}
        try:
            values = client.read(address, count)
            raw.update({address + i: value for i, value in enumerate(values)})
            group["status"] = "ok"
            group["words"] = values
        except (OSError, ModbusError) as error:
            group["status"], group["error"] = "error", str(error)
        group["elapsed_ms"] = round((time.monotonic() - begin) * 1000, 1)
        groups.append(group)
    metrics = [m for m in PROFILE["metrics"] if not grid_only or m["key"] == "gridVoltageV"]
    result = {
        "timestamp": datetime.now(timezone.utc).isoformat(), "profile": PROFILE["id"],
        "host": client.host, "port": client.port, "unit_id": client.unit_id,
        "elapsed_ms": round((time.monotonic() - started) * 1000, 1),
        "modbusRequests": groups,
        "raw": {m["key"]: raw.get(m["register"]) for m in metrics},
        "registers": {m["key"]: m["register"] for m in metrics},
    }
    result.update({m["key"]: decode(m, raw.get(m["register"])) for m in metrics})
    result["ok"] = all(g["status"] == "ok" for g in groups) and all(result[m["key"]] is not None for m in metrics)
    return result


def display(result):
    def value(key):
        number = result.get(key)
        return "unavailable" if number is None else f"{number:.2f}"
    print(f"{result['timestamp']} {result['host']}:{result['port']} unit={result['unit_id']} "
          f"{result['elapsed_ms']} ms status={'ok' if result['ok'] else 'partial/error'}")
    if "acC" in result:
        print(f"Temperature: AC={value('acC')} C, DC={value('dcC')} C, Battery={value('batteryC')} C; "
              f"Battery SOC={value('batteryPercent')}% (CLI only)")
        for key, name in (("pv1", "PV1"), ("pv2", "PV2"), ("grid", "Grid"), ("load", "Load"), ("battery", "Battery")):
            print(f"  {name:7s} {value(key + 'PowerW'):>12s} W  {value(key + 'VoltageV'):>12s} V  {value(key + 'CurrentA'):>12s} A")
    else:
        print(f"Grid: {value('gridVoltageV')} V")
    for group in result["modbusRequests"]:
        print(f"  FC03 {group['address']}..{group['address'] + group['count'] - 1}: "
              f"{group['status']}, {group['elapsed_ms']} ms" + (f" ({group['error']})" if "error" in group else ""))


def run_read(args, settings):
    host, port = settings
    client = Client(args.host or host, port if args.logger_port is None else args.logger_port, args.unit_id, args.timeout)
    if args.count < 1 or not math.isfinite(args.interval) or args.interval < 0.1:
        raise ValueError("count must be >=1 and interval >=0.1 seconds")
    if args.log:
        args.log.parent.mkdir(parents=True, exist_ok=True)
    cycles, failed = 0, False
    while args.watch or cycles < args.count:
        result = poll(client, args.grid_only)
        encoded = json.dumps(result, ensure_ascii=True)
        if args.log:
            with args.log.open("a", encoding="utf-8") as log:
                log.write(encoded + "\n")
        print(encoded) if args.json else display(result)
        failed |= not result["ok"]
        cycles += 1
        if args.watch or cycles < args.count:
            time.sleep(args.interval)
    if failed:
        raise ModbusError("At least one reading was unavailable; inspect the diagnostic log")
