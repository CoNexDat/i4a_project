import hashlib
import importlib.util
import io
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("flash_node", ROOT / "gateway/flash_node.py")
ota = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = ota
spec.loader.exec_module(ota)


def image_bytes():
    image = bytearray(1024)
    image[0] = 0xE9
    struct.pack_into("<I", image, 32, 0xABCD5432)
    image[176:208] = hashlib.sha256(b"test ELF").digest()
    return bytes(image)


class FakeNode:
    """Fault-injecting transport; models ACKs, not flash implementation."""
    def __init__(self, missing=None, reject=None, drop_once=None):
        self.sent = []
        self.queue = []
        self.missing = missing
        self.reject = reject
        self.drop_once = drop_once
        self.dropped = False
        self.booted = False
        self.elf_hash = image_bytes()[176:208]

    def send(self, request):
        self.sent.append(request)
        self.queue = []
        if request.op == ota.REBOOT:
            self.booted = True
        for role in (0, 4):
            if role == self.missing:
                continue
            phase = {ota.BEGIN: 1, ota.DATA: 1, ota.END: 2,
                     ota.SELECT: 3, ota.REBOOT: 3}.get(request.op, 0)
            error = 0x107 if request.op == self.reject and role == 0 else 0
            data = struct.pack("<IB", error, phase)
            if request.op == ota.INFO:
                data += struct.pack("<II", 0x1F0000, 0x210000 if self.booted else 0x20000)
                data += self.elf_hash if self.booted else bytes(32)
                data += struct.pack("<IB", 0, 0)
            offset = request.offset + (len(request.data) if request.op == ota.DATA else 0)
            reply = ota.Packet(request.op | ota.REPLY, request.targets, role,
                               request.session, request.sequence, offset, data)
            if request.op == self.drop_once and not self.dropped and role == 0:
                self.dropped = True
                continue
            self.queue.append(reply)

    def receive(self, deadline):
        return self.queue.pop(0) if self.queue else None


class FakeClock:
    def __init__(self):
        self.now = 0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class ProtocolTests(unittest.TestCase):
    def test_crc_and_length_reject_corruption(self):
        p = ota.Packet(ota.DATA, 0x11, 255, 0x1234, 2, 448, bytes(range(256)))
        self.assertEqual(ota.Packet.decode(p.encode()), p)
        for index in range(len(p.encode())):
            wire = bytearray(p.encode())
            wire[index] ^= 1
            with self.assertRaises(ValueError):
                ota.Packet.decode(wire)
        with self.assertRaises(ValueError):
            ota.Packet.decode(p.encode()[:-1])

    def test_role_inventory_is_explicit(self):
        self.assertEqual(ota.parse_roles("north,center"), 0x11)
        for roles in ("north", "center", "center,north,north", "center,unknown"):
            with self.assertRaises(Exception):
                ota.parse_roles(roles)

    def test_rejects_bootloader_merged_and_wrong_chip_images(self):
        image = image_bytes()
        self.assertEqual(ota.image_elf_hash(image), image[176:208])
        for bad in (b"", bytes(1024), b"\xff" * 4096 + image,
                    image[:12] + b"\x09" + image[13:]):
            with self.assertRaises(ota.UpdateError):
                ota.image_elf_hash(bad)

    def test_line_parser_ignores_logs_and_bad_crc(self):
        reply = ota.Packet(0x81, 0x11, 4, 1, 1, data=bytes(50))
        class Port:
            in_waiting = 0
            data = bytearray(b"boot log\nI4AOTA:bad\n" + ota.PREFIX +
                             reply.encode().hex().encode() + b"\r\n")
            def read(self, size):
                part = bytes(self.data[:3])
                del self.data[:3]
                return part
        transport = object.__new__(ota.SerialTransport)
        transport.port = Port()
        transport.buffer = bytearray()
        transport.log = None
        self.assertEqual(transport.receive(ota.time.monotonic() + 1), reply)

    def test_verbose_serial_preserves_device_diagnostics(self):
        class Port:
            in_waiting = 0
            data = bytearray(b"Guru Meditation Error\nI4AOTA:bad\n")
            def read(self, size):
                part = bytes(self.data)
                self.data.clear()
                return part
        messages = []
        transport = object.__new__(ota.SerialTransport)
        transport.port = Port()
        transport.buffer = bytearray()
        transport.log = messages.append
        self.assertIsNone(transport.receive(ota.time.monotonic() + 0.01))
        self.assertEqual(messages[0], "device: Guru Meditation Error")
        self.assertIn("invalid OTA reply", messages[1])


class UploadTests(unittest.TestCase):
    def updater(self, transport):
        return ota.NodeUpdater(transport, 0x11, timeout=0.01, retries=2, report=lambda _: None)

    def test_automatic_recovery_waits_through_lost_ring_until_clean_boot(self):
        clock = FakeClock()
        transport = FakeNode()
        original_send = transport.send
        def send(request):
            original_send(request)
            if request.op != ota.INFO or not transport.booted:
                return
            if 30 <= clock.now < 140:
                transport.queue = []  # Ring stopped until boards restart locally.
            else:
                transport.queue = [ota.Packet(p.op, p.targets, p.sender, p.session,
                    p.sequence, p.offset, p.data + bytes([clock.now < 140]))
                    for p in transport.queue]
        transport.send = send
        messages = []
        updater = self.updater(transport)
        updater.report = messages.append
        with patch.object(ota.time, "monotonic", clock.monotonic), patch.object(ota.time, "sleep", clock.sleep):
            updater.update(image_bytes())
        self.assertGreaterEqual(clock.now, 140)
        self.assertTrue(any("recovery_pending=1" in m for m in messages))
        self.assertTrue(messages[-1].startswith("Verified:"))
        self.assertEqual(sum(p.op == ota.REBOOT for p in transport.sent), 1)
        self.assertNotIn(ota.ABORT, [p.op for p in transport.sent])

    def test_recovery_pending_blocks_new_upload_even_with_valid_image(self):
        transport = FakeNode()
        original_send = transport.send
        def send(request):
            original_send(request)
            transport.queue = [ota.Packet(p.op, p.targets, p.sender, p.session,
                p.sequence, p.offset, p.data + b"\x01") for p in transport.queue]
        transport.send = send
        with self.assertRaisesRegex(ota.UpdateError, "busy or boot is unconfirmed"):
            self.updater(transport).update(image_bytes())
        self.assertEqual([p.op for p in transport.sent], [ota.INFO])

    def test_recovery_flag_must_clear_on_every_board_before_success(self):
        clock = FakeClock()
        transport = FakeNode()
        original_send = transport.send
        def send(request):
            original_send(request)
            if request.op == ota.INFO and transport.booted:
                transport.queue = [ota.Packet(p.op, p.targets, p.sender, p.session,
                    p.sequence, p.offset, p.data + bytes([p.sender == 0]))
                    for p in transport.queue]
        transport.send = send
        with patch.object(ota.time, "monotonic", clock.monotonic), patch.object(ota.time, "sleep", clock.sleep):
            with self.assertRaisesRegex(ota.UpdateError, "north: recovery_pending=1"):
                self.updater(transport).update(image_bytes(), boot_timeout=24)
        self.assertEqual(clock.now, 24)

    @patch.object(ota.time, "sleep")
    def test_full_flow_verifies_boot_and_preserves_image(self, _sleep):
        transport = FakeNode()
        self.updater(transport).update(image_bytes())
        ops = [p.op for p in transport.sent]
        self.assertLess(ops.index(ota.END), ops.index(ota.SELECT))
        self.assertLess(ops.index(ota.SELECT), ops.index(ota.REBOOT))
        self.assertEqual(ops[-1], ota.INFO)
        chunks = b"".join(p.data for p in transport.sent if p.op == ota.DATA)
        self.assertEqual(chunks, image_bytes())
        begin = next(p for p in transport.sent if p.op == ota.BEGIN)
        self.assertEqual(begin.data[4:], hashlib.sha256(chunks).digest())

    def test_missing_board_never_starts_flash(self):
        transport = FakeNode(missing=0)
        with self.assertRaisesRegex(ota.UpdateError, "north"):
            self.updater(transport).update(image_bytes())
        self.assertTrue(all(p.op == ota.INFO for p in transport.sent))

    def test_flash_error_aborts_without_select_or_reboot(self):
        transport = FakeNode(reject=ota.DATA)
        with self.assertRaises(ota.UpdateError):
            self.updater(transport).update(image_bytes())
        self.assertEqual(transport.sent[-1].op, ota.ABORT)
        self.assertNotIn(ota.SELECT, [p.op for p in transport.sent])
        self.assertNotIn(ota.REBOOT, [p.op for p in transport.sent])

    def test_timeout_identifies_operation_and_offset(self):
        transport = FakeNode(missing=0)
        messages = []
        updater = self.updater(transport)
        updater.trace = messages.append
        with self.assertRaisesRegex(ota.UpdateError, r"north \(DATA seq=1 offset=448, 2 attempts\)"):
            updater.command(ota.DATA, b"test", 448)
        self.assertTrue(any("TX DATA" in message for message in messages))
        self.assertTrue(any("ACK center" in message for message in messages))
        self.assertEqual(transport.sent[0], transport.sent[1])

    def test_verbose_info_keeps_partial_status_when_another_board_is_missing(self):
        transport = FakeNode(missing=0)
        messages = []
        updater = self.updater(transport)
        updater.trace = messages.append
        with self.assertRaisesRegex(ota.UpdateError, "No ACK from: north"):
            updater.status()
        status = next(message for message in messages if message.startswith("STATUS center:"))
        self.assertIn("running=0x20000", status)
        self.assertIn("pending_verify=0", status)
        self.assertIn("elf_sha256=" + bytes(32).hex(), status)

    def test_idle_with_pending_boot_is_not_success_and_identifies_board(self):
        transport = FakeNode()
        original_send = transport.send
        def send(request):
            original_send(request)
            if transport.booted and request.op == ota.INFO:
                reply = transport.queue[0]
                transport.queue[0] = ota.Packet(reply.op, reply.targets, reply.sender,
                    reply.session, reply.sequence, reply.offset, reply.data[:49] + b"\x01")
        transport.send = send
        messages = []
        updater = self.updater(transport)
        updater.report = updater.trace = messages.append
        clock = FakeClock()
        with patch.object(ota.time, "monotonic", clock.monotonic), \
                patch.object(ota.time, "sleep", clock.sleep):
            with self.assertRaisesRegex(ota.UpdateError, "Last complete INFO: north: pending_verify=1"):
                updater.update(image_bytes(), boot_timeout=25)
        self.assertEqual(sum(message.startswith("Boot not confirmed:") for message in messages), 1)
        self.assertTrue(any("running=0x210000, pending_verify=1" in message for message in messages))

    def test_matching_hash_on_old_partition_is_not_success(self):
        transport = FakeNode()
        original_send = transport.send
        def send(request):
            original_send(request)
            if transport.booted and request.op == ota.INFO:
                reply = transport.queue[0]
                data = reply.data[:9] + struct.pack("<I", 0x20000) + reply.data[13:]
                transport.queue[0] = ota.Packet(reply.op, reply.targets, reply.sender,
                    reply.session, reply.sequence, reply.offset, data)
        transport.send = send
        clock = FakeClock()
        with patch.object(ota.time, "monotonic", clock.monotonic), \
                patch.object(ota.time, "sleep", clock.sleep):
            with self.assertRaisesRegex(ota.UpdateError, "north: still on previous partition 0x20000"):
                self.updater(transport).update(image_bytes(), boot_timeout=25)

    def test_boot_deadline_bounds_info_retries_and_preserves_last_mismatch(self):
        transport = FakeNode()
        transport.elf_hash = bytes(32)
        original_send, original_receive = transport.send, transport.receive
        clock = FakeClock()
        boot_queries = []
        def send(request):
            original_send(request)
            if transport.booted and request.op == ota.INFO:
                boot_queries.append(request)
                if len(boot_queries) > 1:
                    transport.queue.clear()
        def receive(deadline):
            if transport.queue:
                return original_receive(deadline)
            clock.now = deadline
            return None
        transport.send, transport.receive = send, receive
        updater = ota.NodeUpdater(transport, 0x11, timeout=30, retries=3, report=lambda _: None)
        with patch.object(ota.time, "monotonic", clock.monotonic), \
                patch.object(ota.time, "sleep", clock.sleep):
            with self.assertRaises(ota.UpdateError) as failure:
                updater.update(image_bytes(), boot_timeout=25)
        self.assertEqual(clock.now, 25)
        self.assertEqual(len(boot_queries), 2)  # one complete INFO, one wait until the deadline
        self.assertIn("Last complete INFO: north: ELF hash mismatch", str(failure.exception))
        self.assertIn("Last INFO error: No ACK", str(failure.exception))
        self.assertIn("1 attempts", str(failure.exception))

    def test_serial_loss_preserves_data_failure_when_abort_also_fails(self):
        transport = FakeNode()
        original_send, original_receive = transport.send, transport.receive
        device_error = OSError("ClearCommError failed (device not functioning, 31)")
        def send(request):
            original_send(request)
            if request.op == ota.ABORT:
                raise OSError("Write timeout")
        def receive(deadline):
            if transport.sent[-1].op == ota.DATA and len(transport.queue) == 1:
                raise device_error  # north ACKed; center ACK was not received
            return original_receive(deadline)
        transport.send, transport.receive = send, receive
        messages = []
        updater = self.updater(transport)
        updater.report = messages.append
        with self.assertRaisesRegex(ota.TransportError, r"DATA seq=3 offset=0; ACK received from: north") as failure:
            updater.update(image_bytes())
        self.assertIs(failure.exception.__cause__, device_error)
        self.assertIn("ClearCommError failed", str(failure.exception))
        self.assertEqual([p.op for p in transport.sent], [ota.INFO, ota.BEGIN, ota.DATA, ota.ABORT])
        self.assertTrue(any("Write timeout" in message for message in messages))
        self.assertTrue(any("SELECT was not sent" in message for message in messages))

    def test_serial_loss_during_select_does_not_claim_old_boot_is_preserved(self):
        transport = FakeNode()
        original_send = transport.send
        def send(request):
            original_send(request)
            if request.op in (ota.SELECT, ota.ABORT):
                raise OSError("Write timeout")
        transport.send = send
        messages = []
        updater = self.updater(transport)
        updater.report = messages.append
        with self.assertRaisesRegex(ota.TransportError, "SELECT"):
            updater.update(image_bytes())
        self.assertNotIn(ota.REBOOT, [p.op for p in transport.sent])
        self.assertFalse(any("SELECT was not sent" in message for message in messages))

    def test_serial_loss_during_reboot_requires_inspection_without_abort(self):
        transport = FakeNode()
        original_send = transport.send
        def send(request):
            original_send(request)
            if request.op == ota.REBOOT:
                raise OSError("Device disconnected")
        transport.send = send
        messages = []
        updater = self.updater(transport)
        updater.report = messages.append
        with self.assertRaisesRegex(ota.TransportError, "REBOOT"):
            updater.update(image_bytes())
        self.assertEqual(transport.sent[-1].op, ota.REBOOT)
        self.assertNotIn(ota.ABORT, [p.op for p in transport.sent])
        self.assertTrue(any("Do not assume the node is updated" in message for message in messages))

    @patch.object(ota.time, "sleep")
    def test_serial_loss_during_boot_verification_is_not_reported_as_rollback(self, _sleep):
        transport = FakeNode()
        original_receive = transport.receive
        def receive(deadline):
            if transport.booted and transport.sent[-1].op == ota.INFO:
                raise OSError("Device disconnected")
            return original_receive(deadline)
        transport.receive = receive
        with self.assertRaisesRegex(ota.TransportError, "INFO.*Device disconnected"):
            self.updater(transport).update(image_bytes())
        self.assertNotIn(ota.ABORT, [p.op for p in transport.sent])

    def test_partial_select_is_canceled(self):
        transport = FakeNode(reject=ota.SELECT)
        with self.assertRaises(ota.UpdateError):
            self.updater(transport).update(image_bytes())
        self.assertEqual(transport.sent[-1].op, ota.ABORT)
        self.assertNotIn(ota.REBOOT, [p.op for p in transport.sent])

    @patch.object(ota.time, "sleep")
    def test_lost_ack_reuses_identical_request(self, _sleep):
        transport = FakeNode(drop_once=ota.DATA)
        self.updater(transport).update(image_bytes())
        chunks = [p for p in transport.sent if p.op == ota.DATA]
        self.assertEqual(chunks[0], chunks[1])

    def test_stale_ack_does_not_satisfy_current_request(self):
        transport = FakeNode(missing=0)
        original_send = transport.send
        def send(request):
            original_send(request)
            transport.queue.insert(0, ota.Packet(request.op | ota.REPLY, request.targets,
                                   0, request.session, request.sequence - 1, data=bytes(50)))
        transport.send = send
        with self.assertRaises(ota.UpdateError):
            self.updater(transport).status()

    @patch.object(ota.time, "sleep")
    def test_uncertain_reboot_is_verified_without_abort(self, _sleep):
        transport = FakeNode(reject=ota.REBOOT)
        self.updater(transport).update(image_bytes())
        self.assertNotIn(ota.ABORT, [p.op for p in transport.sent])
        self.assertEqual(transport.sent[-1].op, ota.INFO)

    @patch.object(ota.time, "sleep")
    def test_old_firmware_after_reboot_is_not_success(self, _sleep):
        transport = FakeNode()
        transport.elf_hash = bytes(32)
        with self.assertRaisesRegex(ota.UpdateError, "Boot could not be verified"):
            self.updater(transport).update(image_bytes(), boot_timeout=0.001)


class CliTests(unittest.TestCase):
    def test_close_failure_preserves_original_diagnostic_and_failure_exit_code(self):
        transport = Mock()
        transport.send.side_effect = OSError("ClearCommError failed")
        transport.close.side_effect = OSError("Device disconnected during close")
        stderr = io.StringIO()
        with patch.object(ota, "SerialTransport", return_value=transport), \
                patch.object(sys, "argv", ["flash_node.py", "--port", "COM5", "--roles",
                                          "center,north", "--status"]), \
                patch.object(sys, "stderr", stderr):
            self.assertEqual(ota.main(), 1)
        self.assertIn("INFO seq=1", stderr.getvalue())
        self.assertIn("ClearCommError failed", stderr.getvalue())
        self.assertIn("Serial port cleanup failed", stderr.getvalue())
        transport.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
