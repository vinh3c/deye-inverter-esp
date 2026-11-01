"""Write logger settings through the firmware console without touching Matter NVS."""
import argparse
import secrets
import sys
import time


def apply_config(port, host, logger_port, *, timeout=25, token=None):
    token = token or secrets.token_hex(4)
    command = f"matter esp logger set {host} {logger_port} {token}\r\n".encode("ascii")
    expected = f"LOGGER_CONFIG_SAVED token={token} host={host} port={logger_port}".encode("ascii")
    deadline = time.monotonic() + timeout
    # Allow the board to boot following flash or USB port re-open.
    send_at = time.monotonic() + 2
    received = b""
    while time.monotonic() < deadline:
        if time.monotonic() >= send_at:
            port.write(command)
            port.flush()
            send_at = time.monotonic() + 3
        received = (received + port.read(4096))[-16384:]
        if expected + b"\n" in received or expected + b"\r\n" in received:
            return
    raise RuntimeError("No config commit acknowledgement from firmware. Check the port and flash firmware with logger config support. Firmware may already have been flashed; pairing was not erased.")


def main():
    import serial

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--host", required=True)
    parser.add_argument("--logger-port", required=True, type=int)
    args = parser.parse_args()
    # No forced reset or erase; DTR/RTS remain deasserted when opening the port.
    with serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=2) as port:
        port.dtr = False
        port.rts = False
        port.port = args.port
        port.open()
        apply_config(port, args.host, args.logger_port)
    print(f"Logger config saved and read back: {args.host}:{args.logger_port} (simulation still active)")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError) as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)
