#!/usr/bin/env python3
"""Exercise the Spectranet/DivMMC IM1 ROMCS handoff through FuseX's M1 bus."""

import ctypes
import os


def main():
    lib = ctypes.CDLL(os.environ.get("FUSEX_LIB", "/opt/fusex/lib/libfusex.so"))
    lib.fuse_init.argtypes = (ctypes.c_int, ctypes.POINTER(ctypes.c_char_p))
    lib.fuse_init.restype = ctypes.c_int
    lib.fuse_end.restype = ctypes.c_int
    lib.spectranet_page.argtypes = (ctypes.c_int,)
    lib.spectranet_downstream_address.argtypes = (ctypes.c_uint16,)
    lib.spectranet_downstream_address.restype = ctypes.c_uint16
    lib.expansion_bus_m1_begin.argtypes = (ctypes.c_uint16,)
    lib.expansion_bus_m1_end.argtypes = (ctypes.c_uint16, ctypes.c_uint8)
    lib.readbyte.argtypes = (ctypes.c_uint16,)
    lib.readbyte.restype = ctypes.c_uint8
    lib.divmmc_is_paged.restype = ctypes.c_int

    args = [
        b"fusex", b"--machine=48", b"--spectranet",
        b"--no-spectranet-disable",
    ]
    native = bool(os.environ.get("FUSEX_NO_DIVMMC"))
    if not native:
        divmmc_rom = os.environ.get("FUSEX_ESXMMC_ROM", "/work/data/ESXMMC.BIN")
        args.extend((b"--divmmc", b"--divmmc-write-protect",
                     f"--divmmc-rom={divmmc_rom}".encode()))
    args.append(b"--no-banner")
    argv = (ctypes.c_char_p * len(args))(*args)
    if lib.fuse_init(len(args), argv):
        raise RuntimeError("fuse_init failed")

    try:
        paged = ctypes.c_int.in_dll(lib, "spectranet_paged")
        lib.spectranet_page(0)
        assert paged.value == 1
        probes = (0x0004, 0x0008, 0x0010, 0x0038, 0x0052, 0x0100)
        own_bytes = tuple(lib.readbyte(address) for address in probes)
        assert lib.spectranet_downstream_address(0x0038) == 0x8038

        if native:
            lib.expansion_bus_m1_begin(0x0038)
            assert tuple(lib.readbyte(a) for a in probes) == own_bytes
            assert lib.spectranet_downstream_address(0x0038) == 0x8038
            lib.expansion_bus_m1_end(0x0038, lib.readbyte(0x0038))
            assert paged.value == 1
            print("NATIVE_0038_UNCHANGED_PASS")
            return

        lib.expansion_bus_m1_begin(0x0038)
        host_bytes = tuple(lib.readbyte(address) for address in probes)
        available = ctypes.c_int.in_dll(lib, "spectranet_available").value
        downstream = lib.spectranet_downstream_address(0x0038)
        print(f"0038 begin: own={own_bytes} host={host_bytes} "
              f"divmmc={lib.divmmc_is_paged()} paged={paged.value} "
              f"available={available} downstream={downstream:04x}")
        assert paged.value == 1
        assert host_bytes != own_bytes, "Spectranet still supplies the host ROM"
        assert lib.spectranet_downstream_address(0x0038) == 0x0038
        lib.expansion_bus_m1_end(0x0038, lib.readbyte(0x0038))

        for address in (0x0039, 0x0040, 0x007C):
            lib.expansion_bus_m1_begin(address)
            assert tuple(lib.readbyte(a) for a in probes) != own_bytes
            lib.expansion_bus_m1_end(address, lib.readbyte(address))
            assert paged.value == 1, "Logical Spectranet paging changed"

        # Exit detection uses opcode bytes, not the old fixed $0052 address.
        lib.expansion_bus_m1_begin(0x2a10)
        assert tuple(lib.readbyte(a) for a in probes) != own_bytes
        lib.expansion_bus_m1_end(0x2a10, 0xfb)  # EI
        assert lib.spectranet_downstream_address(0x0038) == 0x0038
        lib.expansion_bus_m1_begin(0x2a11)
        lib.expansion_bus_m1_end(0x2a11, 0xc9)  # RET opcode fetched
        assert lib.spectranet_downstream_address(0x0038) == 0x0038
        # FuseX executes RET's stack reads before the following M1 begins.
        lib.expansion_bus_m1_begin(0x8000)
        restored_bytes = tuple(lib.readbyte(a) for a in probes)
        print(f"EI/RET completed: restored={restored_bytes} "
              f"divmmc={lib.divmmc_is_paged()} paged={paged.value}")
        assert restored_bytes == own_bytes, "Spectranet ROMCS was not restored"
        assert lib.spectranet_downstream_address(0x0038) == 0x8038
        print("IM1_HANDOFF_PASS")
    finally:
        lib.fuse_end()


if __name__ == "__main__":
    main()
