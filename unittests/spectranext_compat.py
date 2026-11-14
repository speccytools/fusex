#!/usr/bin/env python3
"""Firmware 1.5 compatibility: controller ABI and XFS provenance.

Run with FUSEX_LIB pointing to a pthread-enabled null-UI library.
FUSEX_ROM_DIR selects the bundled ROMs.
"""
import ctypes as C
import os
from pathlib import Path
import struct
import tempfile


def main():
    with tempfile.TemporaryDirectory(prefix='fusex-compat-') as profile:
        os.environ['XDG_CONFIG_HOME'] = profile
        lib = C.CDLL(os.environ['FUSEX_LIB'])
        for name, args, result in (
            ('fuse_init', [C.c_int, C.POINTER(C.c_char_p)], C.c_int),
            ('spectranext_controller_write', [C.c_void_p, C.c_uint16, C.c_uint8], None),
            ('xfs_handle_command', [C.c_void_p], None),
            ('readport_internal', [C.c_uint16], C.c_uint8),
            ('writeport_internal', [C.c_uint16, C.c_uint8], None),
        ):
            fn = getattr(lib, name)
            fn.argtypes, fn.restype = args, result
        roms = os.environ.get('FUSEX_ROM_DIR', str(Path(__file__).resolve().parents[1] / 'roms'))
        args = ['fusex', '--machine=48', '--spectranet', '--no-spectranet-disable',
                '--no-banner', '--rom-dir=' + roms]
        divmmc_rom = os.environ.get('FUSEX_ESXMMC_ROM')
        if divmmc_rom:
            args += ['--divmmc', '--divmmc-write-protect', '--divmmc-rom=' + divmmc_rom]
        else:
            args += ['--no-divmmc']
        argv = [arg.encode() for arg in args]
        assert lib.fuse_init(len(argv), (C.c_char_p * len(argv))(*argv)) == 0
        try:
            expected = int(bool(divmmc_rom))
            assert lib.readport_internal(0x073b) == expected
            for value in (0, 1, 2, 3, 0xff):
                lib.writeport_internal(0x073b, value)
                assert lib.readport_internal(0x073b) == expected, 'status write changed peripheral mode'
            for _ in range(10):
                lib.spectrum_do_frame()
                assert lib.readport_internal(0x073b) == expected, 'floating-bus status'
            print('PASS: compatibility status reflects the actual peripheral and is read-only')
            controller = (C.c_uint8 * 4096).in_dll(lib, 'spectranext_controller')
            assert controller[0xbfc] == 0
            lib.spectranext_controller_write(None, 0xbfc, 99)
            assert controller[0xbfc] == 0, 'host changed read-only operation progress'
            print('PASS: controller progress has the expected offset and is read-only')

            regs = (C.c_uint8 * 4096)()
            def command(cmd, payload=b'', handle=0):
                regs[0], regs[4], regs[6] = cmd, handle, 3
                regs[8:520] = payload.ljust(512, b'\0')
                lib.xfs_handle_command(regs)
                result = struct.unpack('<h', bytes(regs[2:4]))[0]
                assert result >= 0, (cmd, result)
                return result
            command(1, struct.pack('<32s64s288s64s64s', b'xfs', b'ram', b'/', b'', b''))
            command(2, struct.pack('<128sHH', b'/compat.tmp', 0x102, 0o644))
            handle = regs[4]
            command(5, handle=handle)
            regs[0x300:0x320] = bytes([0xff]) * 32
            command(9, b'/compat.tmp\0')
            assert bytes(regs[0x316:0x31b]) == b'\0\0XS\xd1', bytes(regs[0x300:0x320]).hex()
            assert bytes(regs[0x31b:0x320]) == bytes(5), 'stale stat extension data'
            command(9, b'/sys/SPXBR128.TAP\0')
            assert bytes(regs[0x318:0x31b]) == b'XS\xd2', 'ROMFS is not system storage'
            command(6, b'/\0')
            handle = regs[4]
            found = False
            for _ in range(100):
                command(7, handle=handle)
                entry = bytes(regs[0x300:0x700])
                name, tail = entry.split(b'\0', 1)
                if name == b'compat.tmp':
                    assert tail[0] == 0xd1
                    found = True
                    break
                if not name:
                    break
            assert found, 'temporary file absent from directory'
            command(8, handle=handle)
            print('PASS: STAT/READDIR persistent host-file provenance, system ROMFS provenance, clean stat extension')
        finally:
            lib.fuse_end()


if __name__ == '__main__':
    main()
