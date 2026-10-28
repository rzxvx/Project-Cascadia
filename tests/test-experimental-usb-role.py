"""Actual transport C with fake policy callbacks, never a gadget registration."""
import ctypes
import json
from pathlib import Path
import subprocess
from experimental_helpers import native_stage

SOURCE = Path(__file__).resolve().parents[1] / "experiments/p105-usb-role"


def main():
    stage, binary = native_stage(SOURCE, ["cascadia-accessory-role.c", "component-test.c"])
    try:
        subprocess.run([str(binary)], check=True)
    finally:
        stage.cleanup()
    # Validate the same production parser against independently executed 13G36.
    stage, binary = native_stage(SOURCE, ["cascadia-accessory-role.c"], shared=True)
    try:
        authorize_type = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint64,
                                         ctypes.c_uint16, ctypes.c_uint16)
        switch_type = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint64,
                                      ctypes.POINTER(ctypes.c_uint8))

        class Policy(ctypes.Structure):
            _fields_ = [("value_mask", ctypes.c_uint16), ("index_mask", ctypes.c_uint16),
                        ("allow_switch", ctypes.c_bool), ("connection_epoch", ctypes.c_uint64),
                        ("context", ctypes.c_void_p), ("authorize", authorize_type),
                        ("request_switch", switch_type)]

        library = ctypes.CDLL(str(binary))
        setup = library.cascadia_accessory_role_setup
        setup.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                          ctypes.POINTER(Policy), ctypes.POINTER(ctypes.c_uint8)]
        setup.restype = ctypes.c_int
        oracle = json.loads((SOURCE / "original-oracle.json").read_text())
        assert oracle["kernelcache_sha256"] == "8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125"
        for row in oracle["experiments"]:
            calls = []
            gate_error = next((c["synthetic_error"] for c in row["calls"]
                               if c.get("synthetic_error")), 0)

            @authorize_type
            def authorize(_, epoch, value, index):
                assert epoch == 42
                calls.append(("authorize", value, index))
                return gate_error

            @switch_type
            def switch(_, epoch, request):
                assert epoch == 42
                calls.append(("switch", bytes(request[:8]).hex()))
                return 0

            policy = Policy(int(row["mask_first"], 16), int(row["mask_second"], 16),
                            True, 42, None, authorize, switch)
            request = (ctypes.c_uint8 * 8).from_buffer_copy(bytes.fromhex(row["setup"]))
            reply = (ctypes.c_uint8 * 4)(*[0xa5] * 4)
            result = setup(request, 8, ctypes.byref(policy), reply)
            if not row["handled"]:
                assert result == -95 and not calls and bytes(reply) == b"\xa5" * 4
            elif row["response"]:
                assert result == 4 and bytes(reply).hex() == row["response"][0] and not calls
            elif row["status"] == "0x0":
                assert result == 0 and calls == [("authorize", 0x34, 0x78), ("switch", row["setup"])]
                assert row["calls"][-1]["payload"] == row["setup"] and row["calls"][-1]["size"] == 8
            elif gate_error:
                assert result == gate_error and len(calls) == 1 and calls[0][0] == "authorize"
            else:
                assert result == -22 and not calls
        print("Actual USB role C and original 13G36 transport comparisons PASS")
    finally:
        stage.cleanup()


if __name__ == "__main__":
    main()
