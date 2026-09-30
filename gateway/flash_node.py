#!/usr/bin/env python3
"""Update all selected ESP32 roles through the center's USB-UART (Python 3.9+)."""

import argparse
from dataclasses import dataclass
import hashlib
from pathlib import Path
import secrets
import struct
import sys
import time
import zlib

PREFIX = b"I4AOTA:"
HEADER = struct.Struct("<BBBBIIIH")
CHUNK = 448
MAX_PACKET = HEADER.size + CHUNK + 4
ROLES = {"north": 0, "south": 1, "east": 2, "west": 3, "center": 4}
INFO, BEGIN, DATA, END, SELECT, ABORT, REBOOT = range(1, 8)
OP_NAMES = dict(enumerate(("INFO", "BEGIN", "DATA", "END", "SELECT", "ABORT", "REBOOT"), 1))
IDLE, RECEIVING, READY, SELECTED = range(4)
REPLY = 0x80


class UpdateError(Exception):
    pass


class TransportError(UpdateError):
    """The host could not use the connection; this is not an OTA ACK timeout."""


@dataclass(frozen=True)
class Packet:
    op: int
    targets: int
    sender: int
    session: int
    sequence: int
    offset: int = 0
    data: bytes = b""

    def encode(self):
        if len(self.data) > CHUNK:
            raise ValueError("Payload too large")
        wire = HEADER.pack(1, self.op, self.targets, self.sender, self.session,
                           self.sequence, self.offset, len(self.data)) + self.data
        return wire + struct.pack("<I", zlib.crc32(wire))

    @classmethod
    def decode(cls, wire):
        if not HEADER.size + 4 <= len(wire) <= MAX_PACKET:
            raise ValueError("Invalid packet length")
        version, op, targets, sender, session, sequence, offset, size = HEADER.unpack_from(wire)
        if version != 1 or size != len(wire) - HEADER.size - 4:
            raise ValueError("Invalid packet header")
        if zlib.crc32(wire[:-4]) != struct.unpack_from("<I", wire, len(wire) - 4)[0]:
            raise ValueError("Invalid CRC")
        return cls(op, targets, sender, session, sequence, offset, wire[HEADER.size:-4])


class SerialTransport:
    def __init__(self, port, baud, log=None):
        try:
            import serial
        except ImportError as exc:
            raise UpdateError("Install pyserial: python -m pip install -r gateway/requirements-ota.txt") from exc
        # Configure before opening: do not intentionally pulse EN/BOOT via DTR/RTS.
        self.port = serial.Serial(port=None, baudrate=baud, timeout=0.2, write_timeout=5)
        self.port.dtr = False
        self.port.rts = False
        self.port.port = port
        self.port.open()
        self.buffer = bytearray()
        self.log = log

    def send(self, packet):
        self.port.write(b"\n" + PREFIX + packet.encode().hex().encode("ascii") + b"\n")
        self.port.flush()

    def receive(self, deadline):
        while time.monotonic() < deadline:
            while b"\n" in self.buffer:
                line, _, rest = self.buffer.partition(b"\n")
                self.buffer = bytearray(rest)
                start = line.find(PREFIX)
                if start < 0:
                    if self.log and line.strip():
                        self.log("device: " + line.decode("utf-8", errors="replace").rstrip())
                    continue  # normal firmware/bootloader logs share UART0
                try:
                    return Packet.decode(bytes.fromhex(line[start + len(PREFIX):].decode("ascii")))
                except (ValueError, UnicodeError):
                    if self.log:
                        self.log("device: invalid OTA reply: " + line.decode("utf-8", errors="replace"))
                    continue
            self.buffer.extend(self.port.read(max(1, min(self.port.in_waiting, 2048))))
            if len(self.buffer) > 4096:
                self.buffer.clear()
        return None

    def close(self):
        self.port.close()


def role_names(mask):
    return ", ".join(name for name, role in ROLES.items() if mask & (1 << role))


def boot_recovery_pending(reply):
    return len(reply.data) >= 51 and bool(reply.data[50])


def format_status(reply):
    if len(reply.data) not in (50, 51):
        raise UpdateError("Unsupported INFO response")
    capacity, address = struct.unpack_from("<II", reply.data, 5)
    return (f"{role_names(1 << reply.sender)}: phase={reply.data[4]}, slot={capacity}, "
            f"running=0x{address:x}, pending_verify={reply.data[49]}, "
            f"recovery_pending={int(boot_recovery_pending(reply))}, "
            f"session={struct.unpack_from('<I', reply.data, 45)[0]}, "
            f"elf_sha256={reply.data[13:45].hex()}")


def parse_roles(value):
    names = [name.strip().lower() for name in value.split(",")]
    if len(names) != len(set(names)) or any(name not in ROLES for name in names):
        raise argparse.ArgumentTypeError("Use unique roles: north,south,east,west,center")
    mask = sum(1 << ROLES[name] for name in names)
    if not mask & (1 << ROLES["center"]) or len(names) < 2:
        raise argparse.ArgumentTypeError("Include center and at least one peripheral (closed SPI ring)")
    return mask


def image_elf_hash(image):
    # ESP image header (24), first segment header (8), esp_app_desc (256).
    if len(image) < 288 or image[0] != 0xE9 or struct.unpack_from("<H", image, 12)[0] != 0:
        raise UpdateError("Expected an ESP32 application .bin (not a merged flash image or bootloader)")
    if struct.unpack_from("<I", image, 32)[0] != 0xABCD5432:
        raise UpdateError("Missing ESP-IDF application descriptor; use app/build/main.bin")
    return image[176:208]


class NodeUpdater:
    def __init__(self, transport, roles, timeout=30, retries=3, report=print, trace=None):
        self.transport = transport
        self.roles = roles
        self.timeout = timeout
        self.retries = retries
        self.report = report
        self.trace = trace
        self.session = secrets.randbits(32) or 1
        self.sequence = 0

    def command(self, op, data=b"", offset=0, *, deadline=None):
        self.sequence += 1
        request = Packet(op, self.roles, 0xFF, self.session, self.sequence, offset, data)
        replies = {}
        attempts = 0
        operation = f"{OP_NAMES.get(op, op)} seq={self.sequence} offset={offset}"

        def transport_call(function, *args):
            try:
                return function(*args)
            except OSError as exc:
                # pyserial SerialException/SerialTimeoutException inherit OSError.
                # Preserve the failed operation before ABORT advances the sequence.
                acknowledged = role_names(sum(1 << role for role in replies)) or "none"
                raise TransportError(f"Transport I/O failed during {operation}; "
                                     f"ACK received from: {acknowledged}. {exc}") from exc

        for attempt in range(1, self.retries + 1):
            if deadline is not None and time.monotonic() >= deadline:
                break
            attempts += 1
            # Same session/sequence/payload on retry; boards cache successful ACKs.
            if self.trace:
                self.trace(f"TX {operation} attempt={attempt}/{self.retries}")
            transport_call(self.transport.send, request)
            attempt_deadline = time.monotonic() + self.timeout
            if deadline is not None:
                attempt_deadline = min(attempt_deadline, deadline)
            while time.monotonic() < attempt_deadline:
                reply = transport_call(self.transport.receive, attempt_deadline)
                if reply is None:
                    break
                if (reply.op != op | REPLY or reply.targets != self.roles or
                        reply.session != self.session or reply.sequence != self.sequence or
                        reply.sender not in ROLES.values() or not self.roles & (1 << reply.sender)):
                    continue
                if len(reply.data) < 5:
                    continue
                error = struct.unpack_from("<I", reply.data)[0]
                if self.trace:
                    self.trace(f"ACK {role_names(1 << reply.sender)} {operation} "
                               f"phase={reply.data[4]} written={reply.offset} error=0x{error:x}")
                if error:
                    raise UpdateError(f"{role_names(1 << reply.sender)} rejected {operation}: ESP error 0x{error:x}")
                if op == INFO:
                    details = format_status(reply)
                    if self.trace:
                        # Print each board immediately, even if another board
                        # never replies and the whole INFO command times out.
                        self.trace("STATUS " + details)
                expected_phase = {BEGIN: RECEIVING, DATA: RECEIVING, END: READY,
                                  SELECT: SELECTED, ABORT: IDLE, REBOOT: SELECTED}.get(op)
                if expected_phase is not None and reply.data[4] != expected_phase:
                    raise UpdateError("ACK has an unexpected OTA phase")
                expected_offset = offset + len(data) if op == DATA else offset
                if op in (BEGIN, DATA, END, SELECT, REBOOT) and reply.offset != expected_offset:
                    raise UpdateError("ACK has an unexpected written byte count")
                replies[reply.sender] = reply
                seen = sum(1 << role for role in replies)
                if seen == self.roles:
                    return replies
        missing = self.roles & ~sum(1 << role for role in replies)
        raise UpdateError(f"No ACK from: {role_names(missing)} ({operation}, "
                          f"{attempts} attempts). Check roles, ring continuity and OTA firmware on every board")

    def status(self, *, deadline=None):
        return self.command(INFO, deadline=deadline)

    def wait_for_boot(self, expected_hash, previous, timeout):
        deadline = time.monotonic() + timeout
        last_mismatch = None
        last_error = None
        if self.trace:
            self.trace(f"Expected boot elf_sha256={expected_hash.hex()}")
        # Let the central reset the peripherals and broadcast startup information.
        time.sleep(min(20, timeout))
        while time.monotonic() < deadline:
            try:
                replies = self.status(deadline=deadline)
                mismatches = []
                for role, reply in sorted(replies.items()):
                    reasons = []
                    if reply.data[4] != IDLE:
                        reasons.append(f"phase={reply.data[4]}")
                    if reply.data[13:45] != expected_hash:
                        reasons.append("ELF hash mismatch")
                    if reply.data[49]:
                        reasons.append("pending_verify=1")
                    if boot_recovery_pending(reply):
                        reasons.append("recovery_pending=1 (waiting for automatic restart)")
                    if reply.data[9:13] == previous[role].data[9:13]:
                        address = struct.unpack_from("<I", reply.data, 9)[0]
                        reasons.append(f"still on previous partition 0x{address:x}")
                    if reasons:
                        mismatches.append(f"{role_names(1 << role)}: {', '.join(reasons)}")
                if not mismatches:
                    return
                mismatch = "; ".join(mismatches)
                if mismatch != last_mismatch:
                    self.report("Boot not confirmed: " + mismatch)
                last_mismatch = mismatch
                last_error = None
            except TransportError:
                # A broken OS handle cannot verify the reboot. Retrying INFO on
                # that same handle would hide the connection failure as rollback.
                raise
            except UpdateError as exc:
                if str(exc) != last_error and self.trace:
                    self.trace(f"Boot INFO failed: {exc}")
                last_error = str(exc)
            time.sleep(min(2, max(0, deadline - time.monotonic())))
        details = ""
        if last_mismatch:
            details += f" Last complete INFO: {last_mismatch}."
        if last_error:
            details += f" Last INFO error: {last_error}."
        raise UpdateError("Boot could not be verified on every board." + details +
                          " Run --status; a board may have rolled back. Do not assume the node is updated")

    def update(self, image, boot_timeout=240):
        expected_hash = image_elf_hash(image)
        before = self.status()
        for role, reply in before.items():
            capacity = struct.unpack_from("<I", reply.data, 5)[0]
            if capacity < len(image):
                raise UpdateError(f"{role_names(1 << role)}: OTA slot {capacity} bytes, image {len(image)} bytes")
            if reply.data[4] != IDLE or reply.data[49] or boot_recovery_pending(reply):
                raise UpdateError(f"{role_names(1 << role)} is busy or boot is unconfirmed/recovering. "
                                  "Wait for automatic recovery and inspect --status before retrying")
        self.report(f"Uploading {len(image)} bytes to {role_names(self.roles)}")
        selection_started = False
        reboot_sent = False
        try:
            self.command(BEGIN, struct.pack("<I", len(image)) + hashlib.sha256(image).digest())
            last_progress = -1
            for offset in range(0, len(image), CHUNK):
                chunk = image[offset:offset + CHUNK]
                self.command(DATA, chunk, offset)
                progress = (offset + len(chunk)) * 100 // len(image)
                if progress // 5 != last_progress:
                    self.report(f"{progress}%")
                    last_progress = progress // 5
            self.command(END, offset=len(image))
            self.report("Every board verified the image. Selecting next boot partitions.")
            selection_started = True
            self.command(SELECT, offset=len(image))
            reboot_sent = True
            self.command(REBOOT, offset=len(image))
        except (Exception, KeyboardInterrupt) as update_error:
            if reboot_sent:
                if isinstance(update_error, TransportError):
                    self.report("Connection lost during REBOOT; reconnect and inspect --status. "
                                "Do not assume the node is updated.")
                    raise
                # The response may have been lost after the center rebooted.
                # Verify the resulting firmware instead of undoing a live commit.
                self.report("Reboot ACK uncertain; checking which firmware actually booted.")
            else:
                try:
                    self.command(ABORT)
                    self.report("Update canceled; all boards retained their running firmware.")
                except (Exception, KeyboardInterrupt) as abort_error:
                    self.report(f"Cancellation incomplete: {abort_error}. Keep power on for at least 120 seconds and inspect --status.")
                    if not selection_started:
                        self.report("SELECT was not sent; this upload did not select a new boot image. "
                                    "After recovery, restart the upload from the beginning.")
                raise
        self.wait_for_boot(expected_hash, before, boot_timeout)
        self.report("Verified: every selected role booted and confirmed the new firmware.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Center USB-UART, e.g. COM3 or /dev/ttyUSB0")
    parser.add_argument("--roles", required=True, type=parse_roles,
                        help="All physically present roles, e.g. center,north,south,east,west")
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--image", type=Path, help="ESP-IDF application image, normally app/build/main.bin")
    action.add_argument("--status", action="store_true", help="Inspect roles without writing flash")
    parser.add_argument("--baud", type=int, default=115200, help="Must match firmware UART0 console baud")
    parser.add_argument("--timeout", type=float, default=30, help="ACK timeout per attempt (seconds)")
    parser.add_argument("--retries", type=int, default=3)
    parser.add_argument("--boot-timeout", type=float, default=240,
                        help="Boot verification deadline, including automatic recovery restart (seconds)")
    parser.add_argument("--verbose", action="store_true", help="Show commands, ACKs and firmware logs on stderr")
    args = parser.parse_args()
    if args.timeout <= 0 or args.retries < 1 or args.boot_timeout <= 0 or args.baud <= 0:
        parser.error("Timeouts, retries and baud rate must be positive")
    transport = None
    try:
        image = args.image.read_bytes() if args.image else None
        if image is not None:
            image_elf_hash(image)  # validate before touching the serial port
        trace = (lambda message: print(message, file=sys.stderr, flush=True)) if args.verbose else None
        transport = SerialTransport(args.port, args.baud, log=trace)
        updater = NodeUpdater(transport, args.roles, args.timeout, args.retries, trace=trace)
        if args.status:
            for role, reply in sorted(updater.status().items()):
                print(format_status(reply))
        else:
            updater.update(image, args.boot_timeout)
        return 0
    except (Exception, KeyboardInterrupt) as exc:
        print(f"Update failed: {exc or 'interrupted'}", file=sys.stderr)
        return 1
    finally:
        if transport is not None:
            try:
                transport.close()
            except OSError as exc:
                # Cleanup of a disconnected port must not replace the original
                # diagnostic or turn a verified update into an unhandled traceback.
                print(f"Serial port cleanup failed: {exc}", file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
