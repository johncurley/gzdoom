"""Check the macOS diagnostic shim in an isolated Python process, not the engine."""
import ctypes
import json
from pathlib import Path
import sys
import threading


def check(path):
    shim = ctypes.CDLL(str(path))
    libc = ctypes.CDLL('/usr/lib/libSystem.B.dylib')
    control = shim.gzdoom_test_rng
    control.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64),
                        ctypes.POINTER(ctypes.c_uint64)]
    draw = shim.gzdoom_test_rand
    draw.restype = ctypes.c_int
    state, count = ctypes.c_uint64(), ctypes.c_uint64()

    def require(condition):
        if not condition:
            raise RuntimeError('RNG shim control failed')

    libc.srand(12345)
    expected = [libc.rand() for _ in range(8)]
    libc.srand(12345)
    require([draw() for _ in range(8)] == expected)
    require(control(0, ctypes.byref(state), ctypes.byref(count)))
    initial = (state.value, count.value)
    require(not control(0, ctypes.byref(state), ctypes.byref(count)))
    sequence = [draw() for _ in range(8)]
    require(control(1, ctypes.byref(state), ctypes.byref(count)) and count.value == 8)
    other = threading.Thread(target=lambda: [draw() for _ in range(16)])
    other.start()
    other.join()
    require(control(1, ctypes.byref(state), ctypes.byref(count)) and count.value == 8)
    state.value, count.value = initial
    require(control(2, ctypes.byref(state), ctypes.byref(count)))
    require(sequence == [draw() for _ in range(8)])
    require(control(3, ctypes.byref(state), ctypes.byref(count)))
    require(not control(1, ctypes.byref(state), ctypes.byref(count)))
    libc.srand(12345)
    require([draw() for _ in range(8)] == expected)
    return dict(status='PASS', passthrough_before_after=8, paired_draws=8,
                other_thread_calls=16, nested_scope_rejected=True)


if __name__ == '__main__':
    print(json.dumps(check(Path(sys.argv[1]).resolve(strict=True)), indent=2))
