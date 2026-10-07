"""Compile and execute neighbor admission tests against the real C components."""

import argparse
import ctypes
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc", help="GCC/Clang on Linux, TinyCC on Windows")
    args = parser.parse_args()
    build = ROOT / "app/build/native-tests"
    build.mkdir(parents=True, exist_ok=True)
    component = ROOT / "components/integration/neighbor_manager"
    suites = {
        "registry": ("test_symmetry_and_other_neighbors", "test_pending_tie_break_and_confirmed_priority",
                     "test_expiry_renewal_and_stale_sessions", "test_identity_validation",
                     "test_all_simultaneous_endpoint_orders"),
        "manager": ("test_spi_reservations_snapshots_and_release", "test_spi_loss_expiry_and_stale_cleanup",
                    "test_spi_incoming_identity_and_invalid_owner"),
        "session": ("test_identity_admission_both_roles_and_framing", "test_duplicate_and_lost_reservation_never_admit",
                    "test_incompatible_identity_and_incomplete_frames", "test_expired_or_unrenewed_session_stops_transport"),
        "channel": ("test_scan_cooldown_and_channel_independence",),
        "callbacks": ("test_peer_callbacks_preserve_session_order",),
    }
    for suite, tests in suites.items():
        library = build / (f"neighbors_{suite}.dll" if sys.platform == "win32" else f"libneighbors_{suite}.so")
        command = [args.cc, "-shared", "-Wall", "-Werror"]
        if sys.platform != "win32":
            command += ["-fPIC"]
        if sys.platform.startswith("linux"):
            # Socket providers in this library must take precedence over libc.
            command += ["-Wl,-Bsymbolic"]
        for include in (component / "include", ROOT / "tests/native/neighbors/mocks", ROOT / "tests/native/mocks",
                        ROOT / "components/routing/os/include", ROOT / "components/wireless/peer_session/include"):
            command += ["-I", str(include)]
        for include in ("integration/channel_manager/include", "integration/callbacks",
                        "routing/wireless/include", "routing/siblings/include"):
            command += ["-I", str(ROOT / "components" / include)]
        command += [str(ROOT / f"tests/native/neighbors/{suite}_test.c"),
                    str(component / "src/registry.c")]
        if suite == "session":
            command += [str(ROOT / "components/wireless/peer_session/peer_session.c")]
        command += ["-o", str(library)]
        subprocess.run(command, check=True)
        native = ctypes.CDLL(str(library))
        for name in tests:
            test = getattr(native, name)
            test.restype = ctypes.c_int
            line = test()
            if line:
                raise AssertionError(f"{name}: failed at {suite}_test.c:{line}")
            print(f"{name}: OK")


if __name__ == "__main__":
    main()
