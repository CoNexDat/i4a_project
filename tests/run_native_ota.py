"""Compile/run the real C protocol and receiver with mocked ESP-IDF providers."""

import argparse
import ctypes
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "gateway"))
from flash_node import Packet, CHUNK, MAX_PACKET  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc", help="Native C compiler (GCC/Clang on Linux, TinyCC on Windows)")
    args = parser.parse_args()
    build = ROOT / "app/build/native-tests"
    build.mkdir(parents=True, exist_ok=True)
    library = build / ("node_ota_test.dll" if sys.platform == "win32" else "libnode_ota_test.so")
    component = ROOT / "components/integration/node_ota"
    command = [args.cc, "-shared"]
    if sys.platform != "win32":
        command += ["-fPIC", "-fno-builtin"]
    for include in (ROOT / "tests/native/mocks", component / "include", component / "src"):
        command += ["-I", str(include)]
    command += [str(ROOT / "tests/native/receiver_test.c"),
                str(ROOT / "tests/native/exchange_test.c"),
                str(component / "src/ota_receiver.c"), str(component / "src/ota_protocol.c"),
                "-o", str(library)]
    subprocess.run(command, check=True)
    native = ctypes.CDLL(str(library))
    for name in ("test_lifecycle", "test_invalid_order_and_hash", "test_expiry_and_flash_failures",
                 "test_partial_select_failure", "test_info_does_not_change_transaction",
                 "test_exchange_serializes_flash_and_maps_acks", "test_exchange_timeout_then_retry",
                 "test_exchange_reboot_requires_every_successful_ack",
                 "test_boot_recovery_without_ring_and_no_restart_loop",
                 "test_boot_failure_keeps_rollback_deadline"):
        test = getattr(native, name)
        test.restype = ctypes.c_int
        line = test()
        if line:
            source = "exchange_test.c" if name.startswith(("test_exchange_", "test_boot_")) else "receiver_test.c"
            raise AssertionError(f"{name}: failed at {source}:{line}")
        print(f"{name}: OK")
    native.roundtrip.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p]
    native.roundtrip.restype = ctypes.c_int
    output = ctypes.create_string_buffer(MAX_PACKET)
    for size in (0, 1, 36, CHUNK):
        packet = Packet(3, 0x1F, 255, 0x12345678, 0xDEADBEEF, 123456,
                        bytes(i % 256 for i in range(size)))
        wire = packet.encode()
        length = native.roundtrip(wire, len(wire), output)
        assert length == len(wire) and output.raw[:length] == wire, "C/Python wire mismatch"
        for i in range(len(wire)):
            broken = bytearray(wire)
            broken[i] ^= 1
            assert native.roundtrip(bytes(broken), len(broken), output) == 0, "C accepted corruption"
        for length in range(len(wire)):
            assert native.roundtrip(wire, length, output) == 0, "C accepted truncation"
    print("C/Python protocol compatibility, corruption and truncation: OK")


if __name__ == "__main__":
    main()
