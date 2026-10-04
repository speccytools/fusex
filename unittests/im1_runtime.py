#!/usr/bin/env python3
"""Run a Z80 IM1 interrupt with Spectranet and DivMMC both enabled."""

import ctypes
import os

class RegPair(ctypes.Union):
    _fields_ = [("word", ctypes.c_uint16), ("bytes", ctypes.c_uint8 * 2)]


class Processor(ctypes.Structure):
    _fields_ = [
        ("af", RegPair), ("bc", RegPair), ("de", RegPair), ("hl", RegPair),
        ("af_alt", RegPair), ("bc_alt", RegPair),
        ("de_alt", RegPair), ("hl_alt", RegPair),
        ("ix", RegPair), ("iy", RegPair),
        ("i", ctypes.c_uint8), ("r", ctypes.c_uint16),
        ("r7", ctypes.c_uint8),
        ("sp", RegPair), ("pc", RegPair), ("memptr", RegPair),
        ("iff2_read", ctypes.c_int),
        ("iff1", ctypes.c_uint8), ("iff2", ctypes.c_uint8),
        ("im", ctypes.c_uint8), ("halted", ctypes.c_int),
        ("clockl", ctypes.c_uint16), ("clockh", ctypes.c_uint16),
        ("q", ctypes.c_uint8), ("interrupts_enabled_at", ctypes.c_int32),
    ]


def main():
    lib = ctypes.CDLL(os.environ.get("FUSEX_LIB", "/opt/fusex/lib/libfusex.so"))
    lib.fuse_init.argtypes = (ctypes.c_int, ctypes.POINTER(ctypes.c_char_p))
    lib.fuse_init.restype = ctypes.c_int
    lib.fuse_end.restype = ctypes.c_int
    lib.spectranet_page.argtypes = (ctypes.c_int,)
    lib.spectranet_downstream_address.argtypes = (ctypes.c_uint16,)
    lib.spectranet_downstream_address.restype = ctypes.c_uint16
    lib.writebyte_internal.argtypes = (ctypes.c_uint16, ctypes.c_uint8)
    lib.readbyte.argtypes = (ctypes.c_uint16,)
    lib.readbyte.restype = ctypes.c_uint8
    lib.event_add_with_data.argtypes = (ctypes.c_uint32, ctypes.c_int,
                                        ctypes.c_void_p)
    lib.z80_interrupt.restype = ctypes.c_int

    divmmc_rom = os.environ.get("FUSEX_ESXMMC_ROM", "/work/data/ESXMMC.BIN")
    args = [b"fusex", b"--machine=48", b"--spectranet",
            b"--no-spectranet-disable", b"--divmmc",
            b"--divmmc-write-protect", f"--divmmc-rom={divmmc_rom}".encode(),
            b"--no-banner"]
    argv = (ctypes.c_char_p * len(args))(*args)
    if lib.fuse_init(len(args), argv):
        raise RuntimeError("fuse_init failed")

    try:
        cpu = Processor.in_dll(lib, "z80")
        tstates = ctypes.c_uint32.in_dll(lib, "tstates")
        paged = ctypes.c_int.in_dll(lib, "spectranet_paged")

        # An interruptible RAM loop keeps normal opcode fetches above $4000.
        lib.writebyte_internal(0x8000, 0x18)  # JR -2
        lib.writebyte_internal(0x8001, 0xFE)
        cpu.pc.word = 0x8000
        cpu.sp.word = 0x9000
        cpu.im = 1
        cpu.iff1 = 1
        cpu.iff2 = 1
        cpu.halted = 0
        cpu.interrupts_enabled_at = -1
        lib.spectranet_page(0)
        assert paged.value == 1
        tstates.value = 0
        assert lib.z80_interrupt() == 1
        assert cpu.pc.word == 0x0038

        callback_type = ctypes.CFUNCTYPE(None, ctypes.c_uint32,
                                         ctypes.c_int, ctypes.c_void_p)
        callback = callback_type(lambda _time, _type, _data: None)
        step_event = lib.event_register(callback, b"IM1 trace step")
        saw_0038 = False
        saw_0052 = False
        waiting_steps = 0

        for _ in range(100000):
            pc = cpu.pc.word
            released = lib.spectranet_downstream_address(0x0038) == 0x0038
            if pc == 0x0038:
                saw_0038 = True
            if released:
                waiting_steps += 1
            if pc == 0x0052 and released:
                saw_0052 = True

            lib.event_add_with_data(tstates.value + 1, step_event, None)
            lib.z80_do_opcodes()
            lib.event_do_events()

            if saw_0038 and waiting_steps and not released and cpu.pc.word == 0x8000:
                break

        restored = lib.spectranet_downstream_address(0x0038) == 0x8038
        print(f"IM1_RUNTIME saw_0038={int(saw_0038)} "
              f"saw_0052={int(saw_0052)} waiting_steps={waiting_steps} "
              f"pc={cpu.pc.word:04x} paged={paged.value} "
              f"restored={int(restored)}")
        assert saw_0038 and waiting_steps > 0
        assert paged.value == 1 and restored
        assert cpu.pc.word == 0x8000
        print("IM1_RUNTIME_PASS")
    finally:
        lib.fuse_end()


if __name__ == "__main__":
    main()
