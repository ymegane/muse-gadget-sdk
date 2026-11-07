"""Send the Muse SDK token to a board over serial without echoing it.

The token comes from the MUSE_SDK_TOKEN environment variable, or, when that
is unset, from the 1Password CLI (`op read`) at the reference in
MUSE_SDK_TOKEN_REF (for example "op://<vault>/<item>/credential").

The port is opened without toggling DTR/RTS, so the board does not reset.
Only the board's "@sdk_token ..." answer is printed: the log line that
follows shows part of the token, so nothing else from the board is printed.

Usage: python send_sdk_token.py /dev/cu.usbmodem<N>
Needs pyserial (the esptool pipx environment has it).
"""
import os
import subprocess
import sys
import time

import serial


def read_token():
    token = os.environ.get("MUSE_SDK_TOKEN", "").strip()
    if token:
        return token
    ref = os.environ.get("MUSE_SDK_TOKEN_REF", "").strip()
    if not ref:
        sys.exit("set MUSE_SDK_TOKEN, or MUSE_SDK_TOKEN_REF to a 1Password op:// reference")
    try:
        return subprocess.run(["op", "read", "--no-newline", ref], check=True,
                              capture_output=True, text=True).stdout.strip()
    except FileNotFoundError:
        sys.exit("1Password CLI (op) not found: brew install --cask 1password-cli")
    except subprocess.CalledProcessError as e:
        sys.exit("op read failed: " + e.stderr.strip())


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[-2])
    token = read_token()
    if not token:
        sys.exit("empty SDK token")

    s = serial.Serial()
    s.port = sys.argv[1]
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False  # don't reset the board when opening the port
    s.rts = False
    s.open()
    try:
        s.reset_input_buffer()
        s.write(b"\n>sdk_token=" + token.encode() + b"\n")
        s.flush()
        buf = b""
        deadline = time.time() + 10
        while time.time() < deadline:
            buf += s.read(4096)
            for line in buf.split(b"\n"):
                if line.startswith(b"@sdk_token"):
                    print(line.decode(errors="replace").strip())
                    sys.exit(0 if b" ok" in line else 1)
        sys.exit("no @sdk_token answer within 10 s")
    finally:
        s.close()


if __name__ == "__main__":
    main()
