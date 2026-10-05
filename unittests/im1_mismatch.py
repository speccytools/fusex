#!/usr/bin/env python3
"""An unexpected instruction after EI must abort the IM1 handoff."""

import ctypes
import os
import resource
import signal


def trigger_mismatch():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    lib = ctypes.CDLL(os.environ.get("FUSEX_LIB", "/opt/fusex/lib/libfusex.so"))
    lib.fuse_init.argtypes = (ctypes.c_int, ctypes.POINTER(ctypes.c_char_p))
    lib.expansion_bus_m1_begin.argtypes = (ctypes.c_uint16,)
    lib.expansion_bus_m1_end.argtypes = (ctypes.c_uint16, ctypes.c_uint8)

    divmmc_rom = os.environ.get("FUSEX_ESXMMC_ROM", "/work/data/ESXMMC.BIN")
    args = [b"fusex", b"--machine=48", b"--spectranet",
            b"--no-spectranet-disable", b"--divmmc",
            b"--divmmc-write-protect", f"--divmmc-rom={divmmc_rom}".encode(),
            b"--no-banner"]
    argv = (ctypes.c_char_p * len(args))(*args)
    if lib.fuse_init(len(args), argv):
        os._exit(2)

    lib.spectranet_page(0)
    lib.expansion_bus_m1_begin(0x0038)
    lib.expansion_bus_m1_end(0x0038, 0xf5)
    lib.expansion_bus_m1_begin(0x2a10)
    lib.expansion_bus_m1_end(0x2a10, 0xfb)  # EI
    lib.expansion_bus_m1_begin(0x2a11)
    lib.expansion_bus_m1_end(0x2a11, 0x00)  # not RET: fail closed
    os._exit(3)


def main():
    pid = os.fork()
    if pid == 0:
        trigger_mismatch()

    _, status = os.waitpid(pid, 0)
    assert os.WIFSIGNALED(status) and os.WTERMSIG(status) == signal.SIGABRT, status
    print("IM1_MISMATCH_ABORT_PASS")


if __name__ == "__main__":
    main()
