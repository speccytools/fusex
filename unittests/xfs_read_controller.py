#!/usr/bin/env python3
"""Check XFS_READ destinations used by the ROM's mount-independent READ."""
import ctypes
import os
import struct
from pathlib import Path


def main():
    lib = ctypes.CDLL(os.environ.get('FUSEX_LIB', '/opt/fusex/lib/libfusex.so'))
    lib.fuse_init.argtypes = (ctypes.c_int, ctypes.POINTER(ctypes.c_char_p))
    lib.fuse_init.restype = ctypes.c_int
    lib.spectranext_controller_write.argtypes = (
        ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint8)
    lib.spectranet_ram_page.argtypes = (ctypes.c_uint8,)
    lib.spectranet_ram_page.restype = ctypes.POINTER(ctypes.c_uint8)
    args = [b'fusex', b'--machine=48', b'--spectranet',
            b'--no-spectranet-disable', b'--no-banner']
    argv = (ctypes.c_char_p * len(args))(*args)
    assert lib.fuse_init(len(args), argv) == 0
    try:
        controller = (ctypes.c_uint8 * 4096).in_dll(lib, 'spectranext_controller')
        romfs = Path(os.environ.get('FUSEX_ROMFS',
                         str(Path(__file__).resolve().parents[1] / 'roms/spxromfs.bin'))).read_bytes()
        _, _, entry_size, count, data_offset, _ = struct.unpack_from('<IHHIII', romfs)
        for i in range(count):
            start = 20 + i * entry_size
            entry = romfs[start:start + entry_size]
            if entry[:128].split(b'\0')[0] == b'/sys' and \
                    entry[128:192].split(b'\0')[0] == b'browser.tap':
                offset, size = struct.unpack_from('<II', entry, 196)
                expected = romfs[data_offset + offset:data_offset + offset + size]
                break
        else:
            raise AssertionError('browser.tap missing from test ROMFS')

        def read(source, page, offset, size):
            request = struct.pack('<128sIBHI', b'/sys/browser.tap', source,
                                  page, offset, size)
            for i, value in enumerate(request):
                controller[0x800 + i] = value
            controller[0xffe] = 0xff
            lib.spectranext_controller_write(None, 0xffe, 8)
            actual = struct.unpack('<I', bytes(controller[0x88b:0x88f]))[0]
            return controller[0xfff], actual

        # The browser's F_xread repeats this exact 512-byte destination.
        code = bytes(controller[:0x800])
        result = bytearray()
        while len(result) < len(expected):
            status, count = read(len(result), 0x48, 0x900, 512)
            assert status == 0 and count > 0, (status, count, len(result))
            result.extend(controller[0x900:0x900 + count])
        assert result == expected
        assert read(len(expected), 0x48, 0x900, 512) == (0, 0)
        assert bytes(controller[:0x800]) == code

        # Reject writes into the request/code area or past the workspace.
        for page, offset, size in ((0x48, 0x8ff, 1), (0x48, 0xffe, 1),
                                  (0x48, 0xffd, 2), (0x47, 0x900, 1),
                                  (0xdf, 0xfff, 2)):
            assert read(0, page, offset, size)[0] == 1
        assert read(0, 0x48, 0xffd, 1) == (0, 1)
        assert controller[0xffd] == expected[0]
        # Retain ordinary RAM-page reads, including a page crossing.
        assert read(0, 0xc0, 0xff0, 32) == (0, 32)
        first = lib.spectranet_ram_page(0xc0)
        second = lib.spectranet_ram_page(0xc1)
        assert bytes(first[0xff0:0x1000]) + bytes(second[:16]) == expected[:32]
        print('PASS: browser TAP reads, EOF, protected bounds, RAM page crossing')
    finally:
        lib.fuse_end()


if __name__ == '__main__':
    main()
